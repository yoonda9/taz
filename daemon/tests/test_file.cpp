// Unit tests for handlers/file.c: FILE_STAT driven end to end through
// taz_dispatch_frame with a real uv_loop_t, so the handler's work-submit /
// after-work path (taz/work.h) actually runs.

#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "handlers/file.h"
#include "taz/dispatch.h"
#include "taz/frame.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/file.pb.h"

namespace
{

// ---------------------------------------------------------------------------
// Fake dispatch (mirrors test_work.cpp's FakeConn): counts conn_ref/
// conn_unref, exposes a closing flag, stands in for connection.c.
// ---------------------------------------------------------------------------

struct FakeConn
{
    int ref_count = 0;
    int unref_count = 0;
    int closing = 0;
};

void CountRef(void *ctx)
{
    static_cast<FakeConn *>(ctx)->ref_count++;
}

void CountUnref(void *ctx)
{
    static_cast<FakeConn *>(ctx)->unref_count++;
}

int IsClosing(void *ctx)
{
    return static_cast<FakeConn *>(ctx)->closing;
}

struct WriteCtx
{
    std::vector<std::vector<uint8_t>> frames;
};

void capture_write(const uint8_t *data, size_t len, void *ctx)
{
    auto *wctx = static_cast<WriteCtx *>(ctx);
    wctx->frames.emplace_back(data, data + len);
}

taz_frame_header_t unpack_header(const std::vector<uint8_t> &frame)
{
    taz_frame_header_t h{};
    if (frame.size() >= static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        taz_frame_unpack_header(frame.data(), &h);
    }
    return h;
}

std::vector<uint8_t> frame_payload(const std::vector<uint8_t> &frame)
{
    if (frame.size() < static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        return {};
    }
    return {frame.begin() + TAZ_FRAME_HEADER_SIZE, frame.end()};
}

std::vector<uint8_t> encode_stat_request(const std::string &path)
{
    taz_v1_FileStatRequest req = taz_v1_FileStatRequest_init_zero;
    if (!path.empty())
    {
        (void)strncpy(req.path, path.c_str(), sizeof(req.path) - 1U);
    }
    std::vector<uint8_t> buf(taz_v1_FileStatRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_FileStatRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

// ---------------------------------------------------------------------------
// Fixture: real loop, fake dispatch, a scratch directory removed in
// TearDown.
// ---------------------------------------------------------------------------

class FileStatTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_EQ(uv_loop_init(&loop_), 0);

        taz_dispatch_init(&d_);
        d_.loop = &loop_;
        d_.conn_ref = CountRef;
        d_.conn_unref = CountUnref;
        d_.conn_ctx = &conn_;
        d_.conn_closing = IsClosing;

        char tmpdir[1024];
        size_t tmpdir_len = sizeof(tmpdir) - 1U;
        ASSERT_EQ(uv_os_tmpdir(tmpdir, &tmpdir_len), 0);
        const std::string tpl_str =
            std::string(tmpdir, tmpdir_len) + "/taz_file_test_XXXXXX";
        std::vector<char> tpl(tpl_str.begin(), tpl_str.end());
        tpl.push_back('\0');

        uv_fs_t req;
        ASSERT_EQ(uv_fs_mkdtemp(nullptr, &req, tpl.data(), nullptr), 0);
        dir_ = req.path;
        uv_fs_req_cleanup(&req);
    }

    void TearDown() override
    {
        // Every test creates at most a couple of entries directly inside
        // dir_ (a file, a subdirectory, a symlink); no nested trees.
        uv_fs_t scan_req;
        if (uv_fs_scandir(nullptr, &scan_req, dir_.c_str(), 0, nullptr) >= 0)
        {
            uv_dirent_t ent;
            while (uv_fs_scandir_next(&scan_req, &ent) != UV_EOF)
            {
                const std::string child = dir_ + "/" + ent.name;
                uv_fs_t rm_req;
                if (ent.type == UV_DIRENT_DIR)
                {
                    (void)uv_fs_rmdir(nullptr, &rm_req, child.c_str(), nullptr);
                }
                else
                {
                    (void)uv_fs_unlink(nullptr, &rm_req, child.c_str(),
                                       nullptr);
                }
                uv_fs_req_cleanup(&rm_req);
            }
        }
        uv_fs_req_cleanup(&scan_req);

        uv_fs_t rmdir_req;
        (void)uv_fs_rmdir(nullptr, &rmdir_req, dir_.c_str(), nullptr);
        uv_fs_req_cleanup(&rmdir_req);

        ASSERT_EQ(uv_loop_close(&loop_), 0);
    }

    std::string JoinDir(const std::string &name) const
    {
        return dir_ + "/" + name;
    }

    static void WriteFile(const std::string &path, const std::string &contents)
    {
        uv_fs_t open_req;
        const uv_file fd = uv_fs_open(
            nullptr, &open_req, path.c_str(),
            UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644, nullptr);
        uv_fs_req_cleanup(&open_req);
        ASSERT_GE(fd, 0);

        if (!contents.empty())
        {
            const uv_buf_t buf =
                uv_buf_init(const_cast<char *>(contents.data()),
                            static_cast<unsigned int>(contents.size()));
            uv_fs_t write_req;
            const int n =
                uv_fs_write(nullptr, &write_req, fd, &buf, 1, 0, nullptr);
            uv_fs_req_cleanup(&write_req);
            ASSERT_EQ(n, static_cast<int>(contents.size()));
        }

        uv_fs_t close_req;
        (void)uv_fs_close(nullptr, &close_req, fd, nullptr);
        uv_fs_req_cleanup(&close_req);
    }

    // Drives one FILE_STAT REQUEST through taz_dispatch_frame. If
    // check_in_flight is set, it runs right after dispatch returns but
    // before the loop runs, to observe state while the work is still
    // (ostensibly) in flight.
    void
    DispatchStatRequest(const std::vector<uint8_t> &payload, uint32_t stream_id,
                        const std::function<void()> &check_in_flight = nullptr)
    {
        taz_frame_header_t header{};
        header.type = static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST);
        header.flags = static_cast<uint8_t>(taz_v1_FrameFlag_FRAME_FLAG_NONE);
        header.opcode = static_cast<uint16_t>(taz_v1_Opcode_OPCODE_FILE_STAT);
        header.length = static_cast<uint32_t>(payload.size());
        header.stream_id = stream_id;

        taz_dispatch_frame(&d_, &header,
                           payload.empty() ? nullptr : payload.data(),
                           TAZ_FRAME_OK, capture_write, &wctx_);
        if (check_in_flight)
        {
            check_in_flight();
        }
        ASSERT_EQ(uv_run(&loop_, UV_RUN_DEFAULT), 0);
    }

    const std::vector<std::vector<uint8_t>> &Frames() const
    {
        return wctx_.frames;
    }

    size_t ActiveStreamCount() const
    {
        return d_.active_count;
    }

    int RefCount() const
    {
        return conn_.ref_count;
    }

    int UnrefCount() const
    {
        return conn_.unref_count;
    }

  private:
    uv_loop_t loop_{};
    taz_dispatch_t d_{};
    FakeConn conn_{};
    WriteCtx wctx_{};
    std::string dir_;
};

} // namespace

TEST_F(FileStatTest, RegularFileReportsSizeAndKind)
{
    const std::string path = JoinDir("hello.txt");
    WriteFile(path, "hello"); // 5 bytes

    DispatchStatRequest(encode_stat_request(path), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.opcode, static_cast<uint16_t>(taz_v1_Opcode_OPCODE_FILE_STAT));
    EXPECT_EQ(h.stream_id, 1U);

    taz_v1_FileStatResponse resp = taz_v1_FileStatResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_FileStatResponse_fields, &resp));

    EXPECT_EQ(resp.size, 5U);
    EXPECT_EQ(resp.kind, taz_v1_Kind_KIND_FILE);
    EXPECT_STREQ(resp.link_target, "");

    const auto now = static_cast<uint64_t>(time(nullptr));
    const uint64_t delta =
        now >= resp.modified ? now - resp.modified : resp.modified - now;
    EXPECT_LE(delta, 5U);

#ifndef _WIN32
    EXPECT_STRNE(resp.owner, "");
    EXPECT_EQ(resp.permissions, 0644U);
#endif

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileStatTest, DirectoryReportsKindDir)
{
    const std::string path = JoinDir("subdir");
    uv_fs_t mkdir_req;
    ASSERT_EQ(uv_fs_mkdir(nullptr, &mkdir_req, path.c_str(), 0755, nullptr), 0);
    uv_fs_req_cleanup(&mkdir_req);

    DispatchStatRequest(encode_stat_request(path), 2U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_FileStatResponse resp = taz_v1_FileStatResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_FileStatResponse_fields, &resp));
    EXPECT_EQ(resp.kind, taz_v1_Kind_KIND_DIR);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileStatTest, SymlinkReportsKindSymlinkAndTarget)
{
    const std::string target = JoinDir("target.txt");
    WriteFile(target, "x");
    const std::string link = JoinDir("link");

    uv_fs_t symlink_req;
    const int rc = uv_fs_symlink(nullptr, &symlink_req, "target.txt",
                                 link.c_str(), 0, nullptr);
    uv_fs_req_cleanup(&symlink_req);
#ifdef _WIN32
    if (rc == UV_EPERM || rc == UV_EACCES)
    {
        GTEST_SKIP() << "no symlink privilege on this Windows host";
    }
#endif
    ASSERT_EQ(rc, 0);

    DispatchStatRequest(encode_stat_request(link), 3U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_FileStatResponse resp = taz_v1_FileStatResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_FileStatResponse_fields, &resp));
    EXPECT_EQ(resp.kind, taz_v1_Kind_KIND_SYMLINK);
    EXPECT_STREQ(resp.link_target, "target.txt");

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileStatTest, MissingPathReturnsNotFoundWithDetail)
{
    const std::string path = JoinDir("does-not-exist");

    DispatchStatRequest(encode_stat_request(path), 4U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_STRNE(err.detail, "");

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileStatTest, EmptyPathIsInvalidRequestWithoutTouchingThePool)
{
    DispatchStatRequest(encode_stat_request(""), 5U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    // Never queued to the pool: no ref was ever taken.
    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileStatTest, UndecodablePayloadIsInvalidRequest)
{
    // Tag 1 (path), wire type 2 (LEN), announcing a 200-byte string but
    // supplying none: pb_decode must fail on truncated input.
    const std::vector<uint8_t> payload{0x0AU, 0xC8U, 0x01U};

    DispatchStatRequest(payload, 6U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileStatTest, StreamIsActiveAndConnectionRefdWhileInFlight)
{
    const std::string path = JoinDir("hello.txt");
    WriteFile(path, "hello");

    DispatchStatRequest(encode_stat_request(path), 7U,
                        [this]()
                        {
                            EXPECT_EQ(ActiveStreamCount(), 1U);
                            EXPECT_EQ(RefCount(), 1);
                            EXPECT_EQ(UnrefCount(), 0);
                        });

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}
