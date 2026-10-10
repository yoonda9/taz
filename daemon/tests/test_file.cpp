// Unit tests for handlers/file.c: FILE_STAT, FILE_CREATE and FILE_DELETE
// driven end to end through taz_dispatch_frame with a real uv_loop_t, so the
// handlers' work-submit / after-work path (taz/work.h) actually runs.

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "file_test_support.h"
#include "handlers/file.h"
#include "taz/config.h"
#include "taz/log.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/file.pb.h"

namespace
{

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

// FileCreateRequest.content is FT_CALLBACK; encoding it needs an encode
// callback (mirrors handlers/command.c's encode_bytes for *_data fields).
struct BytesCtx
{
    const uint8_t *data;
    size_t len;
};

bool EncodeBytesField(pb_ostream_t *stream, const pb_field_iter_t *field,
                      void *const *arg)
{
    const auto *bctx = static_cast<const BytesCtx *>(*arg);
    if (!pb_encode_tag_for_field(stream, field))
    {
        return false;
    }
    return pb_encode_string(stream, bctx->data, bctx->len);
}

std::vector<uint8_t> encode_create_request(const std::string &path,
                                           const std::string &content,
                                           uint32_t permissions)
{
    taz_v1_FileCreateRequest req = taz_v1_FileCreateRequest_init_zero;
    if (!path.empty())
    {
        (void)strncpy(req.path, path.c_str(), sizeof(req.path) - 1U);
    }
    req.permissions = permissions;

    BytesCtx bctx{reinterpret_cast<const uint8_t *>(content.data()),
                  content.size()};
    if (!content.empty())
    {
        req.content.funcs.encode = EncodeBytesField;
        req.content.arg = &bctx;
    }

    std::vector<uint8_t> buf(sizeof(req.path) + content.size() + 64U);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_FileCreateRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

// Appends a length-delimited field (string/bytes wire type 2) with the given
// field number, varint-encoded length, then raw bytes.
void append_bytes_field(std::vector<uint8_t> &out, uint32_t field_number,
                        const std::string &data)
{
    out.push_back(static_cast<uint8_t>((field_number << 3U) | 2U));
    size_t len = data.size();
    do
    {
        uint8_t byte = static_cast<uint8_t>(len & 0x7FU);
        len >>= 7U;
        if (len != 0U)
        {
            byte |= 0x80U;
        }
        out.push_back(byte);
    } while (len != 0U);
    out.insert(out.end(), data.begin(), data.end());
}

std::vector<uint8_t> encode_delete_request(const std::string &path)
{
    taz_v1_FileDeleteRequest req = taz_v1_FileDeleteRequest_init_zero;
    if (!path.empty())
    {
        (void)strncpy(req.path, path.c_str(), sizeof(req.path) - 1U);
    }
    std::vector<uint8_t> buf(taz_v1_FileDeleteRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_FileDeleteRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

std::vector<uint8_t> encode_chmod_request(const std::string &path,
                                          uint32_t permissions)
{
    taz_v1_FileChmodRequest req = taz_v1_FileChmodRequest_init_zero;
    if (!path.empty())
    {
        (void)strncpy(req.path, path.c_str(), sizeof(req.path) - 1U);
    }
    req.permissions = permissions;
    std::vector<uint8_t> buf(taz_v1_FileChmodRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_FileChmodRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

} // namespace

TEST_F(FileHandlerTest, RegularFileReportsSizeAndKind)
{
    const std::string path = JoinDir("hello.txt");
    WriteFile(path, "hello"); // 5 bytes
    ChmodFile(path, 0644);    // pin the mode regardless of umask

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

    // created == 0 when the platform reports no birth time, else recent.
    if (resp.created != 0U)
    {
        const uint64_t created_delta =
            now >= resp.created ? now - resp.created : resp.created - now;
        EXPECT_LE(created_delta, 5U);
    }

#ifndef _WIN32
    EXPECT_STRNE(resp.owner, "");
    EXPECT_EQ(resp.permissions, 0644U);
#endif

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

#ifdef _WIN32
TEST_F(FileHandlerTest, OwnerIsResolvedViaWindowsSecurityApi)
{
    const std::string path = JoinDir("owned.txt");
    WriteFile(path, "hello");

    DispatchStatRequest(encode_stat_request(path), 100U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_FileStatResponse resp = taz_v1_FileStatResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_FileStatResponse_fields, &resp));

    EXPECT_STRNE(resp.owner, "");
    EXPECT_NE(strchr(resp.owner, '\\'), nullptr);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}
#endif

TEST_F(FileHandlerTest, DirectoryReportsKindDir)
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

TEST_F(FileHandlerTest, SymlinkReportsKindSymlinkAndTarget)
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

TEST_F(FileHandlerTest, MissingPathReturnsNotFoundWithDetail)
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
    EXPECT_STREQ(err.message, "stat failed");
    EXPECT_STRNE(err.detail, "");

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, EmptyPathIsInvalidRequestWithoutTouchingThePool)
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
    EXPECT_STREQ(err.message, "path is required");

    // Never queued to the pool: no ref was ever taken.
    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, EmptyPathFieldInNonEmptyPayloadIsInvalidRequest)
{
    // Tag 1 (path), wire type 2 (LEN), explicit zero length: unlike
    // encode_stat_request(""), this payload is non-empty, so dispatch takes
    // the pb_decode branch (payload != NULL && header->length > 0) rather
    // than the empty-payload shortcut, and still decodes to an empty path.
    const std::vector<uint8_t> payload{0x0AU, 0x00U};

    DispatchStatRequest(payload, 9U);

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

TEST_F(FileHandlerTest, UndecodablePayloadIsInvalidRequest)
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

TEST_F(FileHandlerTest, StreamIsActiveAndConnectionRefdWhileInFlight)
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

TEST_F(FileHandlerTest, ConnectionClosingWhileInFlightSendsNothing)
{
    const std::string path = JoinDir("hello.txt");
    WriteFile(path, "hello");

    // Simulate the connection closing after the work has been handed to the
    // pool but before the loop has run its after-work callback: file_stat_done
    // must still free fctx exactly once, release the stream and unref the
    // connection, but must not write a response (checked by ASan/valgrind
    // for the free; checked here for the behavioural side).
    DispatchStatRequest(encode_stat_request(path), 8U,
                        [this]() { SetConnClosing(1); });

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}

// ---------------------------------------------------------------------------
// FILE_CREATE
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, CreateEmptyContentSucceedsWithZeroSize)
{
    const std::string path = JoinDir("created.txt");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE,
                    encode_create_request(path, "", 0U), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.opcode,
              static_cast<uint16_t>(taz_v1_Opcode_OPCODE_FILE_CREATE));

    taz_v1_FileCreateResponse resp = taz_v1_FileCreateResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_FileCreateResponse_fields, &resp));
    EXPECT_TRUE(resp.success);

    EXPECT_TRUE(PathExists(path));
    EXPECT_EQ(ReadFileBytes(path).size(), 0U);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, CreateWithContentWritesAllBytes)
{
    const std::string path = JoinDir("created.txt");
    const std::string content(3000U, 'x');

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE,
                    encode_create_request(path, content, 0U), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(ReadFileBytes(path), content);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, CreateExistingReturnsAlreadyExists)
{
    const std::string path = JoinDir("created.txt");
    WriteFile(path, "existing");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE,
                    encode_create_request(path, "new", 0U), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type, static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));

    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS);
    EXPECT_EQ(ReadFileBytes(path), "existing");

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, CreateUnderMissingDirectoryReturnsNotFound)
{
    const std::string path = JoinDir("no-such-dir/created.txt");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE,
                    encode_create_request(path, "", 0U), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

#ifndef _WIN32
TEST_F(FileHandlerTest, CreateWithExplicitModeHonoursPermissions)
{
    // An explicit, non-zero permissions field is passed straight to
    // uv_fs_open's mode argument, so the umask still applies to it exactly
    // as it would to a local open(2) call; pin a known umask so the
    // expected mode doesn't depend on the ambient one.
    const ScopedUmask umask_guard(022);
    const std::string path = JoinDir("created.txt");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE,
                    encode_create_request(path, "", 0600U), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(FileMode(path), 0600U);
}

TEST_F(FileHandlerTest, CreateWithZeroPermissionsDefaultsToDefaultMode)
{
    const ScopedUmask umask_guard(022);
    const std::string path = JoinDir("created.txt");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE,
                    encode_create_request(path, "", 0U), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(FileMode(path), 0644U);
}
#endif

TEST_F(FileHandlerTest, CreateEmptyPathIsInvalidRequestWithoutTouchingThePool)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE,
                    encode_create_request("", "", 0U), 1U);

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

TEST_F(FileHandlerTest, CreateUndecodablePayloadIsInvalidRequest)
{
    // Tag 1 (path), wire type 2 (LEN), announcing a 200-byte string but
    // supplying none: pb_decode must fail on truncated input.
    const std::vector<uint8_t> payload{0x0AU, 0xC8U, 0x01U};

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE, payload, 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, CreateWithDuplicateContentFieldKeepsLastContent)
{
    // content (field 2) is FT_CALLBACK; nanopb invokes decode_file_content
    // once per occurrence of the tag on the wire, so a request encoding the
    // field twice must behave like any other proto3 scalar: last occurrence
    // wins. decode_file_content must free the first buffer before replacing
    // it with the second (ASan catches a leak here if it doesn't).
    const std::string path = JoinDir("created.txt");
    std::vector<uint8_t> payload;
    append_bytes_field(payload, 1U, path);
    append_bytes_field(payload, 2U, "aaaa");
    append_bytes_field(payload, 2U, "bbbb");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE, payload, 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(ReadFileBytes(path), "bbbb");

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, CreateConnectionClosingWhileInFlightSendsNothing)
{
    // Mirrors ConnectionClosingWhileInFlightSendsNothing for FILE_STAT, but
    // FILE_CREATE is the only handler that owns a heap buffer (fctx->content)
    // freed in its done() callback on the closing path - exercise it with
    // non-empty content so ASan/valgrind actually cover that free.
    const std::string path = JoinDir("created.txt");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CREATE,
                    encode_create_request(path, "payload-bytes", 0U), 8U,
                    [this]() { SetConnClosing(1); });

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}

// ---------------------------------------------------------------------------
// FILE_DELETE
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest, DeleteRemovesFile)
{
    const std::string path = JoinDir("hello.txt");
    WriteFile(path, "hello");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_DELETE,
                    encode_delete_request(path), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));

    taz_v1_FileDeleteResponse resp = taz_v1_FileDeleteResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_FileDeleteResponse_fields, &resp));
    EXPECT_TRUE(resp.success);
    EXPECT_FALSE(PathExists(path));

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DeleteMissingReturnsNotFound)
{
    const std::string path = JoinDir("does-not-exist");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_DELETE,
                    encode_delete_request(path), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_STRNE(err.detail, "");

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DeleteDirectoryReturnsErrorAndDirectorySurvives)
{
    const std::string path = JoinDir("subdir");
    uv_fs_t mkdir_req;
    ASSERT_EQ(uv_fs_mkdir(nullptr, &mkdir_req, path.c_str(), 0755, nullptr), 0);
    uv_fs_req_cleanup(&mkdir_req);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_DELETE,
                    encode_delete_request(path), 1U);

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

TEST_F(FileHandlerTest, DeleteSymlinkRemovesLinkNotTarget)
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

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_DELETE,
                    encode_delete_request(link), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_FALSE(PathExists(link));
    EXPECT_TRUE(PathExists(target));

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, DeleteEmptyPathIsInvalidRequestWithoutTouchingThePool)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_DELETE, encode_delete_request(""),
                    1U);

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

TEST_F(FileHandlerTest, DeleteUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> payload{0x0AU, 0xC8U, 0x01U};

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_DELETE, payload, 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

// ---------------------------------------------------------------------------
// FILE_CHMOD
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST_F(FileHandlerTest, ChmodSetsExactModeOnPosix)
{
    const std::string path = JoinDir("hello.txt");
    WriteFile(path, "hello");
    ChmodFile(path, 0644);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CHMOD,
                    encode_chmod_request(path, 0600U), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_frame_header_t h = unpack_header(Frames()[0]);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.opcode, static_cast<uint16_t>(taz_v1_Opcode_OPCODE_FILE_CHMOD));

    taz_v1_FileChmodResponse resp = taz_v1_FileChmodResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_FileChmodResponse_fields, &resp));
    EXPECT_TRUE(resp.success);
    EXPECT_EQ(FileMode(path), 0600U);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CHMOD,
                    encode_chmod_request(path, 0644U), 2U);
    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_EQ(FileMode(path), 0644U);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}
#endif

#ifdef _WIN32
TEST_F(FileHandlerTest, ChmodTogglesReadOnlyAttributeOnWindows)
{
    const std::string path = JoinDir("hello.txt");
    WriteFile(path, "hello");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CHMOD,
                    encode_chmod_request(path, 0444U), 1U);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(FileMode(path) & 0222U, 0U);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CHMOD,
                    encode_chmod_request(path, 0644U), 2U);
    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_NE(FileMode(path) & 0200U, 0U);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, ChmodUnrepresentableModeLogsWarningOnWindows)
{
    taz_config_reset();
    taz_log_init();
    taz_log_reset_for_tests();
    const std::string path = JoinDir("hello.txt");
    WriteFile(path, "hello");

    // 0600 drops group/other read, which the READONLY attribute cannot say.
    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CHMOD,
                    encode_chmod_request(path, 0600U), 1U);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));

    taz_v1_LogEntry entries[4];
    ASSERT_EQ(taz_log_collect(0, 0, TAZ_LOG_WARN, entries, 4), 1U);
    EXPECT_STREQ(entries[0].level, "WARN");
    EXPECT_NE(std::string(entries[0].message).find("only partially honoured"),
              std::string::npos);

    taz_log_reset_for_tests();
    taz_config_reset();
}
#endif

TEST_F(FileHandlerTest, ChmodMissingReturnsNotFound)
{
    const std::string path = JoinDir("does-not-exist");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CHMOD,
                    encode_chmod_request(path, 0644U), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_STRNE(err.detail, "");

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, ChmodEmptyPathIsInvalidRequestWithoutTouchingThePool)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CHMOD,
                    encode_chmod_request("", 0644U), 1U);

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

TEST_F(FileHandlerTest, ChmodUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> payload{0x0AU, 0xC8U, 0x01U};

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CHMOD, payload, 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(FileHandlerTest, ChmodConnectionClosingWhileInFlightSendsNothing)
{
    const std::string path = JoinDir("hello.txt");
    WriteFile(path, "hello");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_CHMOD,
                    encode_chmod_request(path, 0600U), 8U,
                    [this]() { SetConnClosing(1); });

    EXPECT_EQ(Frames().size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 1);
    EXPECT_EQ(UnrefCount(), 1);
}
