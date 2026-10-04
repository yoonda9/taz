// Unit tests for error mapping and taz_error_send.

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <uv.h>

#include "taz/error.h"
#include "taz/frame.h"
#include "taz/v1/common.pb.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct WriteCtx
{
    std::vector<std::vector<uint8_t>> frames;
};

void capture_write(const uint8_t *data, size_t len, void *ctx)
{
    auto *wctx = static_cast<WriteCtx *>(ctx);
    wctx->frames.emplace_back(data, data + len);
}

bool DecodeErrorInfo(const std::vector<uint8_t> &frame, taz_v1_ErrorInfo *out)
{
    if (frame.size() <= static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        return false;
    }
    pb_istream_t stream = pb_istream_from_buffer(
        frame.data() + TAZ_FRAME_HEADER_SIZE,
        frame.size() - static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));
    return pb_decode(&stream, taz_v1_ErrorInfo_fields, out);
}

// ---------------------------------------------------------------------------
// taz_error_from_errno
// ---------------------------------------------------------------------------

TEST(ErrorFromErrno, EnoentMapsToNotFound)
{
    EXPECT_EQ(taz_error_from_errno(ENOENT),
              taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

TEST(ErrorFromErrno, EaccesMapsToPerm)
{
    EXPECT_EQ(taz_error_from_errno(EACCES),
              taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED);
}

TEST(ErrorFromErrno, EpermMapsToPerm)
{
    EXPECT_EQ(taz_error_from_errno(EPERM),
              taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED);
}

TEST(ErrorFromErrno, EexistMapsToAlreadyExists)
{
    EXPECT_EQ(taz_error_from_errno(EEXIST),
              taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS);
}

TEST(ErrorFromErrno, EsrchMapsToNotFound)
{
    EXPECT_EQ(taz_error_from_errno(ESRCH),
              taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

TEST(ErrorFromErrno, UnknownMapsToInternal)
{
    EXPECT_EQ(taz_error_from_errno(ERANGE),
              taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
}

TEST(ErrorFromErrno, EnotdirMapsToNotFound)
{
    EXPECT_EQ(taz_error_from_errno(ENOTDIR),
              taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

// ---------------------------------------------------------------------------
// taz_error_from_win32 (Windows only)
// ---------------------------------------------------------------------------

#ifdef _WIN32

TEST(ErrorFromWin32, FileNotFoundMapsToNotFound)
{
    EXPECT_EQ(taz_error_from_win32(ERROR_FILE_NOT_FOUND),
              taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

TEST(ErrorFromWin32, PathNotFoundMapsToNotFound)
{
    EXPECT_EQ(taz_error_from_win32(ERROR_PATH_NOT_FOUND),
              taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
}

TEST(ErrorFromWin32, AccessDeniedMapsToPermissionDenied)
{
    EXPECT_EQ(taz_error_from_win32(ERROR_ACCESS_DENIED),
              taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED);
}

TEST(ErrorFromWin32, AlreadyExistsMapsToAlreadyExists)
{
    EXPECT_EQ(taz_error_from_win32(ERROR_ALREADY_EXISTS),
              taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS);
}

TEST(ErrorFromWin32, FileExistsMapsToAlreadyExists)
{
    EXPECT_EQ(taz_error_from_win32(ERROR_FILE_EXISTS),
              taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS);
}

TEST(ErrorFromWin32, UnknownMapsToInternal)
{
    EXPECT_EQ(taz_error_from_win32(ERROR_NOT_SUPPORTED),
              taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
}

#endif /* _WIN32 */

// ---------------------------------------------------------------------------
// taz_error_from_fs_req / taz_error_fs_detail
// ---------------------------------------------------------------------------

class ErrorFromFsReq : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        char tmpdir[1024];
        size_t tmpdir_len = sizeof(tmpdir) - 1;
        ASSERT_EQ(uv_os_tmpdir(tmpdir, &tmpdir_len), 0);
        const std::string tpl_str =
            std::string(tmpdir, tmpdir_len) + "/taz_error_test_XXXXXX";
        std::vector<char> tpl(tpl_str.begin(), tpl_str.end());
        tpl.push_back('\0');

        uv_fs_t req;
        const int rc = uv_fs_mkdtemp(NULL, &req, tpl.data(), NULL);
        if (rc != 0)
        {
            uv_fs_req_cleanup(&req);
        }
        ASSERT_EQ(rc, 0);
        dir_ = req.path;
        uv_fs_req_cleanup(&req);
    }

    void TearDown() override
    {
        // Best-effort cleanup; individual tests remove what they create.
        uv_fs_t req;
        (void)uv_fs_rmdir(NULL, &req, dir_.c_str(), NULL);
        uv_fs_req_cleanup(&req);
    }

    const std::string &Dir() const
    {
        return dir_;
    }

  private:
    std::string dir_;
};

TEST_F(ErrorFromFsReq, StatMissingPathMapsToNotFound)
{
    const std::string missing = Dir() + "/does-not-exist";
    uv_fs_t req;
    const int rc = uv_fs_lstat(NULL, &req, missing.c_str(), NULL);
    ASSERT_LT(rc, 0);
    ASSERT_LT(req.result, 0);

    EXPECT_EQ(taz_error_from_fs_req(&req),
              taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_STRNE(taz_error_fs_detail(&req), "");
    EXPECT_STREQ(taz_error_fs_detail(&req), uv_strerror((int)req.result));
    uv_fs_req_cleanup(&req);
}

TEST_F(ErrorFromFsReq, MkdirExistingDirectoryMapsToAlreadyExists)
{
    uv_fs_t req;
    const int rc = uv_fs_mkdir(NULL, &req, Dir().c_str(), 0755, NULL);
    ASSERT_LT(rc, 0);
    ASSERT_LT(req.result, 0);

    EXPECT_EQ(taz_error_from_fs_req(&req),
              taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS);
    EXPECT_STRNE(taz_error_fs_detail(&req), "");
    uv_fs_req_cleanup(&req);
}

TEST_F(ErrorFromFsReq, OpenExclExistingFileMapsToAlreadyExists)
{
    const std::string path = Dir() + "/existing-file";
    uv_fs_t create_req;
    const int fd = uv_fs_open(NULL, &create_req, path.c_str(),
                              UV_FS_O_WRONLY | UV_FS_O_CREAT, 0644, NULL);
    ASSERT_GE(fd, 0);
    uv_fs_req_cleanup(&create_req);
    uv_fs_t close_req;
    ASSERT_EQ(uv_fs_close(NULL, &close_req, fd, NULL), 0);
    uv_fs_req_cleanup(&close_req);

    uv_fs_t req;
    const int rc =
        uv_fs_open(NULL, &req, path.c_str(),
                   UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_EXCL, 0644, NULL);
    ASSERT_LT(rc, 0);
    ASSERT_LT(req.result, 0);

    EXPECT_EQ(taz_error_from_fs_req(&req),
              taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS);
    EXPECT_STRNE(taz_error_fs_detail(&req), "");
    uv_fs_req_cleanup(&req);

    uv_fs_t unlink_req;
    (void)uv_fs_unlink(NULL, &unlink_req, path.c_str(), NULL);
    uv_fs_req_cleanup(&unlink_req);
}

TEST_F(ErrorFromFsReq, RmdirNonEmptyDirectoryMapsToInternal)
{
    const std::string sub = Dir() + "/nonempty";
    uv_fs_t mkdir_req;
    ASSERT_EQ(uv_fs_mkdir(NULL, &mkdir_req, sub.c_str(), 0755, NULL), 0);
    uv_fs_req_cleanup(&mkdir_req);
    const std::string child = sub + "/child";
    uv_fs_t open_req;
    const int fd = uv_fs_open(NULL, &open_req, child.c_str(),
                              UV_FS_O_WRONLY | UV_FS_O_CREAT, 0644, NULL);
    ASSERT_GE(fd, 0);
    uv_fs_req_cleanup(&open_req);
    uv_fs_t close_req;
    ASSERT_EQ(uv_fs_close(NULL, &close_req, fd, NULL), 0);
    uv_fs_req_cleanup(&close_req);

    uv_fs_t req;
    const int rc = uv_fs_rmdir(NULL, &req, sub.c_str(), NULL);
    ASSERT_LT(rc, 0);
    ASSERT_LT(req.result, 0);

    EXPECT_EQ(taz_error_from_fs_req(&req),
              taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
    EXPECT_STRNE(taz_error_fs_detail(&req), "");
    uv_fs_req_cleanup(&req);

    uv_fs_t unlink_req;
    (void)uv_fs_unlink(NULL, &unlink_req, child.c_str(), NULL);
    uv_fs_req_cleanup(&unlink_req);
    uv_fs_t rmdir_req;
    (void)uv_fs_rmdir(NULL, &rmdir_req, sub.c_str(), NULL);
    uv_fs_req_cleanup(&rmdir_req);
}

TEST_F(ErrorFromFsReq, UnlinkDirectoryMapsToExpectedCode)
{
    uv_fs_t req;
    const int rc = uv_fs_unlink(NULL, &req, Dir().c_str(), NULL);
    ASSERT_LT(rc, 0);
    ASSERT_LT(req.result, 0);

#ifdef _WIN32
    EXPECT_EQ(taz_error_from_fs_req(&req),
              taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED);
#else
    EXPECT_EQ(taz_error_from_fs_req(&req),
              taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
#endif
    EXPECT_STRNE(taz_error_fs_detail(&req), "");
    EXPECT_STREQ(taz_error_fs_detail(&req), uv_strerror((int)req.result));
    uv_fs_req_cleanup(&req);
}

// ---------------------------------------------------------------------------
// taz_error_send
// ---------------------------------------------------------------------------

TEST(ErrorSend, SendsErrorFrameWithCorrectFields)
{
    WriteCtx wctx;
    taz_error_send(capture_write, &wctx, 7U, 3U,
                   taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND, "not found", NULL);

    ASSERT_EQ(wctx.frames.size(), 1U);
    const auto &frame = wctx.frames[0];
    ASSERT_GE(frame.size(), static_cast<size_t>(TAZ_FRAME_HEADER_SIZE));

    taz_frame_header_t h{};
    taz_frame_unpack_header(frame.data(), &h);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    EXPECT_EQ(h.stream_id, 7U);
    EXPECT_EQ(h.opcode, 3U);
    EXPECT_GT(h.length, 0U);

    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(frame, &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_STREQ(info.message, "not found");
}

TEST(ErrorSend, SendsDetailField)
{
    WriteCtx wctx;
    taz_error_send(capture_write, &wctx, 1U, 0U,
                   taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "internal", "detail");

    ASSERT_EQ(wctx.frames.size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(wctx.frames[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
    EXPECT_STREQ(info.detail, "detail");
}

TEST(ErrorSend, NullMessageAndDetailAreEmptyStrings)
{
    WriteCtx wctx;
    taz_error_send(capture_write, &wctx, 2U, 0U,
                   taz_v1_ErrorCode_ERROR_CODE_BUSY, NULL, NULL);

    ASSERT_EQ(wctx.frames.size(), 1U);
    taz_v1_ErrorInfo info = taz_v1_ErrorInfo_init_zero;
    ASSERT_TRUE(DecodeErrorInfo(wctx.frames[0], &info));
    EXPECT_EQ(info.code, taz_v1_ErrorCode_ERROR_CODE_BUSY);
    EXPECT_STREQ(info.message, "");
    EXPECT_STREQ(info.detail, "");
}

} // namespace
