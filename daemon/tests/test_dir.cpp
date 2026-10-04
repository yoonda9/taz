// Unit tests for handlers/file.c's DIR_MAKE handler, driven end to end
// through taz_dispatch_frame with a real uv_loop_t (see file_test_support.h
// for the shared fixture, also used by test_file.cpp).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "file_test_support.h"
#include "handlers/file.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/file.pb.h"

namespace
{

std::vector<uint8_t> encode_dir_make_request(const std::string &path,
                                             uint32_t permissions, bool parents)
{
    taz_v1_DirMakeRequest req = taz_v1_DirMakeRequest_init_zero;
    if (!path.empty())
    {
        (void)strncpy(req.path, path.c_str(), sizeof(req.path) - 1U);
    }
    req.permissions = permissions;
    req.parents = parents;
    std::vector<uint8_t> buf(taz_v1_DirMakeRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_DirMakeRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

std::vector<uint8_t> encode_dir_list_request(const std::string &path,
                                             bool include_hidden)
{
    taz_v1_DirListRequest req = taz_v1_DirListRequest_init_zero;
    if (!path.empty())
    {
        (void)strncpy(req.path, path.c_str(), sizeof(req.path) - 1U);
    }
    req.include_hidden = include_hidden;
    std::vector<uint8_t> buf(taz_v1_DirListRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_DirListRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

// Callback-based mirror of taz_v1_DirListResponse with no static-array cap
// on entry count (same pattern as test_response.cpp's DirListResponseCb,
// redeclared here since PB_BIND's generated symbols must stay scoped to
// this anonymous namespace per TU). A single RESPONSE frame may legitimately
// pack more entries than one encode batch (64) holds, since the frame
// splitter only cares about byte size, not the handler's own batch size -
// decoding with the real taz_v1_DirListResponse (array of 64) would
// overflow in that case.
struct DirListResponseCb
{
    pb_callback_t entries;
};

// clang-format off
#define DirListResponseCb_FIELDLIST(X, a) \
    X(a, CALLBACK, REPEATED, MESSAGE, entries, 1)
// clang-format on

#define DirListResponseCb_DEFAULT         NULL
#define DirListResponseCb_CALLBACK        pb_default_field_callback
#define DirListResponseCb_entries_MSGTYPE taz_v1_DirEntry

PB_BIND(DirListResponseCb, DirListResponseCb, AUTO)

bool collect_dir_entries(pb_istream_t *stream, const pb_field_iter_t *field,
                         void **arg)
{
    (void)field;
    auto *entries = static_cast<std::vector<taz_v1_DirEntry> *>(*arg);
    taz_v1_DirEntry entry = taz_v1_DirEntry_init_zero;
    if (!pb_decode(stream, taz_v1_DirEntry_fields, &entry))
    {
        return false;
    }
    entries->push_back(entry);
    return true;
}

// Decodes every captured frame as a DirListResponse via the callback
// variant above and returns the concatenated entries in frame order.
std::vector<taz_v1_DirEntry>
decode_all_dir_entries(const std::vector<std::vector<uint8_t>> &frames)
{
    std::vector<taz_v1_DirEntry> entries;
    for (const auto &frame : frames)
    {
        const std::vector<uint8_t> body = frame_payload(frame);
        DirListResponseCb resp{};
        resp.entries.funcs.decode = collect_dir_entries;
        resp.entries.arg = &entries;
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        EXPECT_TRUE(pb_decode(&istream, &DirListResponseCb_msg, &resp));
    }
    return entries;
}

} // namespace

TEST_F(FileHandlerTest, DirMakeCreatesDirectory)
{
    const std::string path = JoinDir("subdir");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.opcode, static_cast<uint16_t>(taz_v1_Opcode_OPCODE_DIR_MAKE));

    taz_v1_DirMakeResponse resp = taz_v1_DirMakeResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_DirMakeResponse_fields, &resp));
    EXPECT_TRUE(resp.success);

#ifndef _WIN32
    EXPECT_EQ(FileMode(path) & 0700U, 0700U);
#endif
    EXPECT_TRUE(PathIsDir(path));

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

#ifndef _WIN32
TEST_F(FileHandlerTest, DirMakeWithExplicitPermissionsHonoursMode)
{
    const ScopedUmask umask_guard(022);
    const std::string path = JoinDir("subdir");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0700U, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(FileMode(path), 0700U);
}
#endif

TEST_F(FileHandlerTest, DirMakeExistingReturnsAlreadyExists)
{
    const std::string path = JoinDir("subdir");
    uv_fs_t mkdir_req;
    ASSERT_EQ(uv_fs_mkdir(nullptr, &mkdir_req, path.c_str(), 0755, nullptr), 0);
    uv_fs_req_cleanup(&mkdir_req);

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirMakeWithoutParentsAndMissingParentReturnsNotFound)
{
    const std::string path = JoinDir("a/b/c");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirMakeWithParentsCreatesAllMissingComponents)
{
    const std::string path = JoinDir("a/b/c");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, true), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DirMakeResponse resp = taz_v1_DirMakeResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_DirMakeResponse_fields, &resp));
    EXPECT_TRUE(resp.success);

    EXPECT_TRUE(PathIsDir(JoinDir("a")));
    EXPECT_TRUE(PathIsDir(JoinDir("a/b")));
    EXPECT_TRUE(PathIsDir(JoinDir("a/b/c")));

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirMakeWithParentsOnExistingDirectorySucceeds)
{
    const std::string path = JoinDir("a/b");
    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, true), 1U);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, true), 2U);
    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_EQ(unpack_header(Frames()[1]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    taz_v1_DirMakeResponse resp = taz_v1_DirMakeResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[1]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_DirMakeResponse_fields, &resp));
    EXPECT_TRUE(resp.success);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest,
       DirMakeWithParentsWhereComponentIsFileReturnsErrorNotNotFound)
{
    const std::string a_path = JoinDir("a");
    WriteFile(a_path, "not a directory");
    const std::string path = JoinDir("a/b");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, true), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirMakeWithoutParentsThroughFileReturnsNotFound)
{
    const std::string a_path = JoinDir("a");
    WriteFile(a_path, "not a directory");
    const std::string path = JoinDir("a/sub");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirMakeWithParentsThroughSymlinkedDirSucceeds)
{
    const std::string real_path = JoinDir("real");
    uv_fs_t mkdir_req;
    ASSERT_EQ(
        uv_fs_mkdir(nullptr, &mkdir_req, real_path.c_str(), 0755, nullptr), 0);
    uv_fs_req_cleanup(&mkdir_req);

    const std::string link_path = JoinDir("link");
    uv_fs_t symlink_req;
    const int rc = uv_fs_symlink(nullptr, &symlink_req, "real",
                                 link_path.c_str(), UV_FS_SYMLINK_DIR, nullptr);
    uv_fs_req_cleanup(&symlink_req);
#ifdef _WIN32
    if (rc == UV_EPERM || rc == UV_EACCES)
    {
        GTEST_SKIP() << "no symlink privilege on this Windows host";
    }
#endif
    ASSERT_EQ(rc, 0);

    const std::string path = JoinDir("link/sub");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, true), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));

    taz_v1_DirMakeResponse resp = taz_v1_DirMakeResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_DirMakeResponse_fields, &resp));
    EXPECT_TRUE(resp.success);

    EXPECT_TRUE(PathIsDir(JoinDir("link/sub")));
    EXPECT_TRUE(PathIsDir(JoinDir("real/sub")));

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirMakeWithParentsOnExistingSymlinkedDirSucceeds)
{
    const std::string real_path = JoinDir("real");
    uv_fs_t mkdir_req;
    ASSERT_EQ(
        uv_fs_mkdir(nullptr, &mkdir_req, real_path.c_str(), 0755, nullptr), 0);
    uv_fs_req_cleanup(&mkdir_req);

    const std::string link_path = JoinDir("link");
    uv_fs_t symlink_req;
    const int rc = uv_fs_symlink(nullptr, &symlink_req, "real",
                                 link_path.c_str(), UV_FS_SYMLINK_DIR, nullptr);
    uv_fs_req_cleanup(&symlink_req);
#ifdef _WIN32
    if (rc == UV_EPERM || rc == UV_EACCES)
    {
        GTEST_SKIP() << "no symlink privilege on this Windows host";
    }
#endif
    ASSERT_EQ(rc, 0);

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(link_path, 0U, true), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));

    taz_v1_DirMakeResponse resp = taz_v1_DirMakeResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_DirMakeResponse_fields, &resp));
    EXPECT_TRUE(resp.success);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirMakeWithoutParentsAcceptsTrailingSeparator)
{
    const std::string path = JoinDir("subdir") + "/";

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_TRUE(PathExists(JoinDir("subdir")));

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirMakeWithParentsAcceptsTrailingSeparator)
{
    const std::string path = JoinDir("a/b") + "/";

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, true), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_TRUE(PathExists(JoinDir("a/b")));

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

#ifdef _WIN32
TEST_F(FileHandlerTest, DirMakeWithParentsAndBackslashDriveSucceeds)
{
    const std::string path = Dir() + "\\x\\y";

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, true), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_TRUE(PathExists(Dir() + "\\x"));
    EXPECT_TRUE(PathExists(Dir() + "\\x\\y"));

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}
#endif

TEST_F(FileHandlerTest, DirMakeEmptyPathIsInvalidRequestWithoutTouchingThePool)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request("", 0U, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_STREQ(err.message, "path is required");

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, DirMakeUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> payload{0x0AU, 0xC8U, 0x01U};

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE, payload, 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, DirMakeConnectionClosingWhileInFlightSendsNothing)
{
    const std::string path = JoinDir("subdir");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_MAKE,
                    encode_dir_make_request(path, 0U, false), 8U,
                    [this]() { SetConnClosing(1); });

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}

TEST_F(FileHandlerTest, DirListReturnsEntriesWithKindsAndSizes)
{
    WriteFile(JoinDir("file.txt"), "1234567");
    uv_fs_t mkdir_req;
    ASSERT_EQ(
        uv_fs_mkdir(nullptr, &mkdir_req, JoinDir("sub").c_str(), 0755, nullptr),
        0);
    uv_fs_req_cleanup(&mkdir_req);
    WriteFile(JoinDir(".hidden"), "x");

    bool have_symlink = false;
    {
        uv_fs_t symlink_req;
        const int rc = uv_fs_symlink(nullptr, &symlink_req, "file.txt",
                                     JoinDir("link").c_str(), 0, nullptr);
        uv_fs_req_cleanup(&symlink_req);
#ifdef _WIN32
        if (rc == UV_EPERM || rc == UV_EACCES)
        {
            have_symlink = false;
        }
        else
        {
            ASSERT_EQ(rc, 0);
            have_symlink = true;
        }
#else
        ASSERT_EQ(rc, 0);
        have_symlink = true;
#endif
    }

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(Dir(), false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));

    taz_v1_DirListResponse resp = taz_v1_DirListResponse_init_zero;
    {
        const std::vector<uint8_t> body = frame_payload(Frames()[0]);
        pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
        ASSERT_TRUE(pb_decode(&istream, taz_v1_DirListResponse_fields, &resp));
    }

    std::map<std::string, std::pair<taz_v1_Kind, uint64_t>> by_name;
    for (pb_size_t i = 0; i < resp.entries_count; i++)
    {
        by_name[resp.entries[i].name] = {resp.entries[i].kind,
                                         resp.entries[i].size};
    }

    ASSERT_EQ(by_name.count("file.txt"), 1U);
    EXPECT_EQ(by_name["file.txt"].first, taz_v1_Kind_KIND_FILE);
    EXPECT_EQ(by_name["file.txt"].second, 7U);

    ASSERT_EQ(by_name.count("sub"), 1U);
    EXPECT_EQ(by_name["sub"].first, taz_v1_Kind_KIND_DIR);
    EXPECT_EQ(by_name["sub"].second, 0U);

    EXPECT_EQ(by_name.count(".hidden"), 0U);

    if (have_symlink)
    {
        ASSERT_EQ(by_name.count("link"), 1U);
        EXPECT_EQ(by_name["link"].first, taz_v1_Kind_KIND_SYMLINK);
        EXPECT_EQ(by_name["link"].second, 0U);
    }

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirListOfSymlinkToDirectoryListsTargetEntries)
{
    const std::string real_path = JoinDir("real");
    uv_fs_t mkdir_req;
    ASSERT_EQ(
        uv_fs_mkdir(nullptr, &mkdir_req, real_path.c_str(), 0755, nullptr), 0);
    uv_fs_req_cleanup(&mkdir_req);
    WriteFile(JoinDir("real/file.txt"), "hi");

    const std::string link_path = JoinDir("link");
    uv_fs_t symlink_req;
    const int rc = uv_fs_symlink(nullptr, &symlink_req, "real",
                                 link_path.c_str(), UV_FS_SYMLINK_DIR, nullptr);
    uv_fs_req_cleanup(&symlink_req);
#ifdef _WIN32
    if (rc == UV_EPERM || rc == UV_EACCES)
    {
        GTEST_SKIP() << "no symlink privilege on this Windows host";
    }
#endif
    ASSERT_EQ(rc, 0);

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(link_path, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));

    taz_v1_DirListResponse resp = taz_v1_DirListResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_DirListResponse_fields, &resp));

    ASSERT_EQ(resp.entries_count, 1);
    EXPECT_STREQ(resp.entries[0].name, "file.txt");
    EXPECT_EQ(resp.entries[0].kind, taz_v1_Kind_KIND_FILE);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirListOfDanglingSymlinkReturnsNotFound)
{
    const std::string link_path = JoinDir("link");
    uv_fs_t symlink_req;
    const int rc = uv_fs_symlink(nullptr, &symlink_req, "does-not-exist",
                                 link_path.c_str(), 0, nullptr);
    uv_fs_req_cleanup(&symlink_req);
#ifdef _WIN32
    if (rc == UV_EPERM || rc == UV_EACCES)
    {
        GTEST_SKIP() << "no symlink privilege on this Windows host";
    }
#endif
    ASSERT_EQ(rc, 0);

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(link_path, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirListIncludesHiddenEntriesOnlyWhenRequested)
{
    WriteFile(JoinDir(".hidden"), "x");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(Dir(), true), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_DirListResponse resp = taz_v1_DirListResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_DirListResponse_fields, &resp));

    bool found = false;
    for (pb_size_t i = 0; i < resp.entries_count; i++)
    {
        if (std::strcmp(resp.entries[i].name, ".hidden") == 0)
        {
            found = true;
        }
    }
    EXPECT_TRUE(found);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest,
       DirListOfEmptyDirectoryReturnsOneResponseWithZeroEntries)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(Dir(), false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.flags & static_cast<uint8_t>(
                            taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
              0U);

    taz_v1_DirListResponse resp = taz_v1_DirListResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_DirListResponse_fields, &resp));
    EXPECT_EQ(resp.entries_count, 0);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirListWithManyFilesSpansMultipleFramesWithContinuation)
{
    static const int kFileCount = 5000;
    // Non-empty content so DirEntry.size is a 2-byte varint (proto3 omits a
    // zero-value scalar entirely): without this, 5000 short-named empty
    // files encode under the 64 KiB single-frame limit and never exercise
    // the chunker this test is for.
    const std::string content(200U, 'x');
    std::set<std::string> expected_names;
    for (int i = 0; i < kFileCount; i++)
    {
        char name[16];
        (void)std::snprintf(name, sizeof(name), "f%04d", i);
        WriteFile(JoinDir(name), content);
        expected_names.insert(name);
    }

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(Dir(), false), 1U);

    ASSERT_GT(Frames().size(), 1U);

    for (size_t i = 0U; i + 1U < Frames().size(); i++)
    {
        const taz_frame_header_t h = unpack_header(Frames()[i]);
        EXPECT_NE(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U)
            << "frame " << i;
    }
    {
        const taz_frame_header_t h = unpack_header(Frames().back());
        EXPECT_EQ(h.flags & static_cast<uint8_t>(
                                taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION),
                  0U);
    }

    const std::vector<taz_v1_DirEntry> entries =
        decode_all_dir_entries(Frames());
    std::set<std::string> actual_names;
    for (const auto &e : entries)
    {
        actual_names.insert(e.name);
    }
    EXPECT_EQ(actual_names.size(), static_cast<size_t>(kFileCount));
    EXPECT_EQ(actual_names, expected_names);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirListMissingPathReturnsNotFound)
{
    const std::string path = JoinDir("missing");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(path, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DirListPathIsFileReturnsErrorNotNotFound)
{
    const std::string path = JoinDir("file.txt");
    WriteFile(path, "hi");

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(path, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_NE(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_TRUE(PathExists(path));

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

#ifndef _WIN32
// Probe for mem-1791147651-a703 / review.rejected on ab80693: a directory
// readable but not searchable (0600: r without x) makes scandir succeed
// (it only needs read on the directory itself) while lstat-ing each
// surviving entry fails with EACCES (search permission is required to
// resolve a name inside the directory). dir_list_work must only treat
// ENOENT from that lstat as "vanished, skip"; any other error (EACCES
// here) must fail the whole listing instead of silently reporting zero
// entries.
TEST_F(FileHandlerTest, DirListWithUnsearchableDirectoryReturnsPermissionDenied)
{
    if (geteuid() == 0)
    {
        GTEST_SKIP() << "root bypasses directory search permission checks";
    }

    const std::string sub = JoinDir("sub");
    uv_fs_t mkdir_req;
    ASSERT_EQ(uv_fs_mkdir(nullptr, &mkdir_req, sub.c_str(), 0755, nullptr), 0);
    uv_fs_req_cleanup(&mkdir_req);
    WriteFile(sub + "/a.txt", "hi");

    const ScopedChmod unsearchable(sub, 0600);

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(sub, false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}
#endif

TEST_F(FileHandlerTest, DirListEmptyPathIsInvalidRequestWithoutTouchingThePool)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request("", false), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_STREQ(err.message, "path is required");

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, DirListUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> payload{0x0AU, 0xC8U, 0x01U};

    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST, payload, 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, DirListConnectionClosingWhileInFlightSendsNothing)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_DIR_LIST,
                    encode_dir_list_request(Dir(), false), 8U,
                    [this]() { SetConnClosing(1); });

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}
