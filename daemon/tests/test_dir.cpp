// Unit tests for handlers/file.c's DIR_MAKE handler, driven end to end
// through taz_dispatch_frame with a real uv_loop_t (see file_test_support.h
// for the shared fixture, also used by test_file.cpp).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

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
    EXPECT_NE(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

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
