// Unit tests for handlers/file_transfer.c: FILE_PUT driven end to end through
// taz_dispatch_frame with a real uv_loop_t, so the multi-step work-submit /
// after-work path (taz/work.h's taz_work_submit_step) actually runs.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "file_test_support.h"
#include "handlers/file_transfer.h"
#include "taz/crc32c.h"
#include "taz/fsutil.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/file.pb.h"

namespace
{

std::vector<uint8_t> encode_put_request(const std::string &dest, uint64_t size,
                                        uint32_t permissions, bool overwrite)
{
    taz_v1_FilePutRequest req = taz_v1_FilePutRequest_init_zero;
    if (!dest.empty())
    {
        (void)strncpy(req.dest, dest.c_str(), sizeof(req.dest) - 1U);
    }
    req.size = size;
    req.permissions = permissions;
    req.overwrite = overwrite;
    std::vector<uint8_t> buf(taz_v1_FilePutRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_FilePutRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

taz_v1_FilePutResponse decode_put_response(const std::vector<uint8_t> &frame)
{
    taz_v1_FilePutResponse resp = taz_v1_FilePutResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(frame);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    EXPECT_TRUE(pb_decode(&istream, taz_v1_FilePutResponse_fields, &resp));
    return resp;
}

taz_v1_ErrorInfo decode_error(const std::vector<uint8_t> &frame)
{
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(frame);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    EXPECT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    return err;
}

void ExpectAck(const std::vector<uint8_t> &frame)
{
    const taz_frame_header_t h = unpack_header(frame);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.opcode, static_cast<uint16_t>(taz_v1_Opcode_OPCODE_FILE_PUT));

    const taz_v1_FilePutResponse resp = decode_put_response(frame);
    EXPECT_EQ(resp.which_phase, taz_v1_FilePutResponse_ack_tag);
    EXPECT_TRUE(resp.phase.ack.ready);
}

void ExpectConfirm(const std::vector<uint8_t> &frame, uint64_t bytes_written,
                   const std::string &content)
{
    const taz_frame_header_t h = unpack_header(frame);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.opcode, static_cast<uint16_t>(taz_v1_Opcode_OPCODE_FILE_PUT));

    const taz_v1_FilePutResponse resp = decode_put_response(frame);
    ASSERT_EQ(resp.which_phase, taz_v1_FilePutResponse_confirm_tag);
    EXPECT_EQ(resp.phase.confirm.bytes_written, bytes_written);

    const uint32_t crc = taz_crc32c(
        reinterpret_cast<const uint8_t *>(content.data()), content.size());
    uint8_t expected[4];
    taz_crc32c_to_le(crc, expected);
    ASSERT_EQ(resp.phase.confirm.checksum.size, 4U);
    EXPECT_EQ(std::memcmp(resp.phase.confirm.checksum.bytes, expected, 4U), 0);
}

std::vector<uint8_t> ToBytes(const std::string &s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> encode_cancel_request(uint32_t target_stream_id)
{
    taz_v1_CancelRequest req = taz_v1_CancelRequest_init_zero;
    req.target_stream_id = target_stream_id;
    std::vector<uint8_t> buf(taz_v1_CancelRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_CancelRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

void ExpectCancelled(const std::vector<uint8_t> &frame, bool cancelled)
{
    const taz_frame_header_t h = unpack_header(frame);
    EXPECT_EQ(h.type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    EXPECT_EQ(h.opcode, static_cast<uint16_t>(taz_v1_Opcode_OPCODE_CANCEL));

    taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(frame);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    EXPECT_TRUE(pb_decode(&istream, taz_v1_CancelResponse_fields, &resp));
    EXPECT_EQ(resp.cancelled, cancelled);
}

} // namespace

TEST_F(FileHandlerTest, PutFourByteFileInOneFinalChunk)
{
    const std::string dest = JoinDir("four.txt");
    const std::string content = "abcd";

    char *temp = taz_fsutil_temp_name(dest.c_str(), 1U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, content.size(), 0U, false), 1U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    DispatchChunk(1U, ToBytes(content), /*last=*/true);
    ASSERT_EQ(Frames().size(), 2U);
    ExpectConfirm(Frames()[1], content.size(), content);

    EXPECT_EQ(ReadFileBytes(dest), content);
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutTwoHundredKiBInSixtyFourKiBChunks)
{
    const std::string dest = JoinDir("big.txt");
    const size_t chunk_size = static_cast<size_t>(64U) * 1024U;
    const size_t total = static_cast<size_t>(200U) * 1024U;
    std::string content;
    content.reserve(total);
    for (size_t i = 0U; i < total; ++i)
    {
        content.push_back(static_cast<char>('A' + (i % 26U)));
    }

    char *temp = taz_fsutil_temp_name(dest.c_str(), 2U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, content.size(), 0U, false), 2U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    size_t sent = 0U;
    while (sent < content.size())
    {
        const size_t n = std::min(chunk_size, content.size() - sent);
        const std::vector<uint8_t> bytes(
            content.begin() + static_cast<long>(sent),
            content.begin() + static_cast<long>(sent + n));
        sent += n;
        DispatchChunk(2U, bytes, /*last=*/sent == content.size());
    }

    ASSERT_EQ(Frames().size(), 2U);
    ExpectConfirm(Frames()[1], content.size(), content);
    EXPECT_EQ(ReadFileBytes(dest), content);
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutZeroSizeWithOneEmptyFinalChunk)
{
    const std::string dest = JoinDir("empty.txt");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 0U, 0U, false), 3U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    DispatchChunk(3U, {}, /*last=*/true);
    ASSERT_EQ(Frames().size(), 2U);
    ExpectConfirm(Frames()[1], 0U, "");

    EXPECT_TRUE(PathExists(dest));
    EXPECT_EQ(ReadFileBytes(dest).size(), 0U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutChunksDispatchedBeforeAckStillLand)
{
    const std::string dest = JoinDir("preack.txt");
    const std::string content = "hello world";

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_FILE_PUT,
                         encode_put_request(dest, content.size(), 0U, false),
                         4U);
    DispatchChunkNoRun(4U, ToBytes(content), /*last=*/true);
    RunLoop();

    ASSERT_EQ(Frames().size(), 2U);
    ExpectAck(Frames()[0]);
    ExpectConfirm(Frames()[1], content.size(), content);
    EXPECT_EQ(ReadFileBytes(dest), content);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

#ifndef _WIN32
TEST_F(FileHandlerTest, PutWithExplicitPermissionsHonoursMode)
{
    const ScopedUmask umask_guard(0);
    const std::string dest = JoinDir("mode600.txt");
    const std::string content = "x";

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, content.size(), 0600U, false), 5U);
    DispatchChunk(5U, ToBytes(content), /*last=*/true);

    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_EQ(FileMode(dest), 0600U);
}

TEST_F(FileHandlerTest, PutWithZeroPermissionsDefaultsToDefaultMode)
{
    const ScopedUmask umask_guard(0);
    const std::string dest = JoinDir("mode644.txt");
    const std::string content = "x";

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, content.size(), 0U, false), 6U);
    DispatchChunk(6U, ToBytes(content), /*last=*/true);

    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_EQ(FileMode(dest), 0644U);
}
#endif

TEST_F(FileHandlerTest, PutOverwriteFalseOntoExistingFileReturnsAlreadyExists)
{
    const std::string dest = JoinDir("existing.txt");
    WriteFile(dest, "original");

    char *temp = taz_fsutil_temp_name(dest.c_str(), 7U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 3U, 0U, false), 7U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_v1_ErrorInfo err = decode_error(Frames()[0]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_ALREADY_EXISTS);

    EXPECT_EQ(ReadFileBytes(dest), "original");
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutOverwriteTrueReplacesExistingBytes)
{
    const std::string dest = JoinDir("existing.txt");
    WriteFile(dest, "original-longer-content");
    const std::string content = "new";

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, content.size(), 0U, true), 8U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    DispatchChunk(8U, ToBytes(content), /*last=*/true);
    ASSERT_EQ(Frames().size(), 2U);
    ExpectConfirm(Frames()[1], content.size(), content);
    EXPECT_EQ(ReadFileBytes(dest), content);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest,
       PutDestIsDirectoryReturnsInvalidRequestRegardlessOfOverwrite)
{
    const std::string dest = JoinDir("adir");
    uv_fs_t req;
    ASSERT_EQ(uv_fs_mkdir(nullptr, &req, dest.c_str(), 0755, nullptr), 0);
    uv_fs_req_cleanup(&req);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 1U, 0U, false), 9U);
    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_ErrorInfo err = decode_error(Frames()[0]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_STREQ(err.message, "destination is a directory");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 1U, 0U, true), 10U);
    ASSERT_EQ(Frames().size(), 2U);
    err = decode_error(Frames()[1]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_STREQ(err.message, "destination is a directory");

    EXPECT_TRUE(PathIsDir(dest));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutParentMissingReturnsNotFound)
{
    const std::string dest = JoinDir("nosuchdir/file.txt");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 1U, 0U, false), 11U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_v1_ErrorInfo err = decode_error(Frames()[0]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutParentIsRegularFileReturnsNotFound)
{
    const std::string parent = JoinDir("notadir.txt");
    WriteFile(parent, "x");
    const std::string dest = parent + "/file.txt";

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 1U, 0U, false), 12U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_v1_ErrorInfo err = decode_error(Frames()[0]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutEmptyDestIsInvalidRequestWithoutTouchingThePool)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request("", 1U, 0U, false), 13U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_v1_ErrorInfo err = decode_error(Frames()[0]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_STREQ(err.message, "dest is required");
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(UnrefCount(), 0);
}

TEST_F(FileHandlerTest, PutUndecodablePayloadIsInvalidRequest)
{
    const std::vector<uint8_t> garbage = {0xFFU, 0xFFU, 0xFFU};

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT, garbage, 14U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_v1_ErrorInfo err = decode_error(Frames()[0]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(UnrefCount(), 0);
}

TEST_F(FileHandlerTest, PutExistingTempReturnsBusyAndLeavesItAlone)
{
    const std::string dest = JoinDir("busy.txt");
    char *temp = taz_fsutil_temp_name(dest.c_str(), 15U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);
    WriteFile(temp_path, "someone-elses-temp");

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 1U, 0U, false), 15U);

    ASSERT_EQ(Frames().size(), 1U);
    const taz_v1_ErrorInfo err = decode_error(Frames()[0]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_BUSY);
    EXPECT_STREQ(err.detail, temp_path.c_str());

    EXPECT_EQ(ReadFileBytes(temp_path), "someone-elses-temp");
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest,
       PutDuplicateRequestOnReceivingStreamIsInvalidRequestAndUploadCompletes)
{
    const std::string dest = JoinDir("dup.txt");
    const std::string content = "hello";

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, content.size(), 0U, false), 16U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, content.size(), 0U, false), 16U);
    ASSERT_EQ(Frames().size(), 2U);
    const taz_v1_ErrorInfo err = decode_error(Frames()[1]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_STREQ(err.message, "duplicate stream_id");

    DispatchChunk(16U, ToBytes(content), /*last=*/true);
    ASSERT_EQ(Frames().size(), 3U);
    ExpectConfirm(Frames()[2], content.size(), content);
    EXPECT_EQ(ReadFileBytes(dest), content);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutIngressPauseAndResumeAroundHighWaterMark)
{
    const std::string dest = JoinDir("backpressure.txt");
    const size_t chunk_size = static_cast<size_t>(64U) * 1024U;
    const size_t total = chunk_size * 5U;
    std::string content(total, 'z');

    EnablePauseResumeCounting();

    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_FILE_PUT,
                         encode_put_request(dest, content.size(), 0U, false),
                         17U);

    size_t sent = 0U;
    while (sent < content.size())
    {
        const std::vector<uint8_t> bytes(
            content.begin() + static_cast<long>(sent),
            content.begin() + static_cast<long>(sent + chunk_size));
        sent += chunk_size;
        DispatchChunkNoRun(17U, bytes, /*last=*/sent == content.size());
    }

    EXPECT_EQ(PauseCalls(), 1);
    EXPECT_EQ(ResumeCalls(), 0);

    RunLoop();

    ASSERT_EQ(Frames().size(), 2U);
    ExpectAck(Frames()[0]);
    ExpectConfirm(Frames()[1], content.size(), content);
    EXPECT_EQ(ReadFileBytes(dest), content);
    EXPECT_EQ(PauseCalls(), 1);
    EXPECT_EQ(ResumeCalls(), 1);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutMoreBytesThanAnnouncedErrorsImmediatelyThenDrains)
{
    const std::string dest = JoinDir("overflow.txt");
    char *temp = taz_fsutil_temp_name(dest.c_str(), 20U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 10U, 0U, false), 20U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    // Announce 10, send 16 in one chunk with CONTINUATION set.
    DispatchChunk(20U, std::vector<uint8_t>(16U, 'x'), /*last=*/false);
    ASSERT_EQ(Frames().size(), 2U);
    {
        const taz_v1_ErrorInfo err = decode_error(Frames()[1]);
        EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
        EXPECT_STREQ(err.message, "more bytes than announced size");
    }
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 1U);

    // A further non-final chunk is dropped: no new frame, temp still gone.
    DispatchChunk(20U, ToBytes("more"), /*last=*/false);
    EXPECT_EQ(Frames().size(), 2U);
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_EQ(ActiveStreamCount(), 1U);

    // The final chunk releases the stream.
    DispatchChunk(20U, {}, /*last=*/true);
    EXPECT_EQ(Frames().size(), 2U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutFewerBytesThanAnnouncedErrorsOnFinalChunkAndReleases)
{
    const std::string dest = JoinDir("short.txt");
    char *temp = taz_fsutil_temp_name(dest.c_str(), 21U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 10U, 0U, false), 21U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    // Announce 10, final chunk carries only 4.
    DispatchChunk(21U, ToBytes("abcd"), /*last=*/true);
    ASSERT_EQ(Frames().size(), 2U);
    const taz_v1_ErrorInfo err = decode_error(Frames()[1]);
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_STREQ(err.message, "received 4 bytes, announced 10");
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

#ifndef _WIN32
TEST_F(FileHandlerTest, PutWriteFailureUnderFsizeLimitErrorsInternalAndDrains)
{
    const ScopedSignalIgnore ignore_sigxfsz(SIGXFSZ);
    const ScopedFsizeLimit fsize_limit(static_cast<rlim_t>(64U) * 1024U);

    const std::string dest = JoinDir("toolarge.txt");
    const size_t chunk_size = static_cast<size_t>(64U) * 1024U;
    char *temp = taz_fsutil_temp_name(dest.c_str(), 22U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(
                        dest, static_cast<uint64_t>(256U) * 1024U, 0U, false),
                    22U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    // First write lands exactly at the fsize limit and succeeds.
    DispatchChunk(22U, std::vector<uint8_t>(chunk_size, 'a'), /*last=*/false);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_TRUE(PathExists(temp_path));

    // Second write starts at the limit: EFBIG.
    DispatchChunk(22U, std::vector<uint8_t>(chunk_size, 'b'), /*last=*/false);
    ASSERT_EQ(Frames().size(), 2U);
    {
        const taz_v1_ErrorInfo err = decode_error(Frames()[1]);
        EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
        EXPECT_STREQ(err.detail, uv_strerror(UV_EFBIG));
    }
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 1U);

    // Later chunks are dropped while DRAINING.
    DispatchChunk(22U, std::vector<uint8_t>(chunk_size, 'c'), /*last=*/false);
    EXPECT_EQ(Frames().size(), 2U);
    EXPECT_EQ(ActiveStreamCount(), 1U);

    DispatchChunk(22U, {}, /*last=*/true);
    EXPECT_EQ(Frames().size(), 2U);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
    EXPECT_FALSE(PathExists(dest));
}
#endif

TEST_F(FileHandlerTest,
       PutConnectionClosingMidUploadAbortsLeavesNoFileAndReleases)
{
    const std::string dest = JoinDir("closemid.txt");
    const size_t chunk_size = static_cast<size_t>(64U) * 1024U;
    char *temp = taz_fsutil_temp_name(dest.c_str(), 23U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, chunk_size * 4U, 0U, false), 23U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    DispatchChunk(23U, std::vector<uint8_t>(chunk_size, 'a'), /*last=*/false);
    DispatchChunk(23U, std::vector<uint8_t>(chunk_size, 'b'), /*last=*/false);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_TRUE(PathExists(temp_path));

    CloseConnectionAndCancelAll();
    RunLoop();

    EXPECT_EQ(Frames().size(), 1U);
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutConnectionClosingWhileWriteInFlightAbortsAndReleases)
{
    const std::string dest = JoinDir("closeinflight.txt");
    const size_t chunk_size = static_cast<size_t>(64U) * 1024U;
    char *temp = taz_fsutil_temp_name(dest.c_str(), 24U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, chunk_size * 2U, 0U, false), 24U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    // Dispatch without running the loop: the write step is submitted to
    // the pool but has not completed, so the close below lands while
    // work_in_flight is still true.
    DispatchChunkNoRun(24U, std::vector<uint8_t>(chunk_size, 'a'),
                       /*last=*/false);
    CloseConnectionAndCancelAll();
    RunLoop();

    EXPECT_EQ(Frames().size(), 1U);
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

// Regression for a swap-remove hazard in taz_dispatch_cancel_all: stream A
// (lower array index) reaches DRAINING, whose abort() frees it
// synchronously via taz_dispatch_stream_done, which swap-removes by moving
// the last active entry into A's just-freed slot. Stream B (added after A,
// so it starts out as that last entry) is still RECEIVING. If cancel_all
// indexes the mutating active_streams[] array instead of re-looking up each
// id it snapshotted up front, B gets swapped into A's freed slot and its own
// abort() is skipped by the loop's increment - leaking B's put_ctx_t, fd,
// and temp file.
TEST_F(FileHandlerTest,
       CancelAllAbortsLiveStreamSwappedIntoDrainingStreamsFreedSlot)
{
    const std::string dest_a = JoinDir("drain_a.txt");
    const std::string dest_b = JoinDir("live_b.txt");
    const size_t chunk_size = static_cast<size_t>(64U) * 1024U;

    char *temp_b = taz_fsutil_temp_name(dest_b.c_str(), 51U);
    ASSERT_NE(temp_b, nullptr);
    const std::string temp_b_path(temp_b);
    free(temp_b);

    // Stream A: added first (array index 0). Announce 10 bytes, send 16 in
    // one chunk so put_on_chunk synchronously triggers put_trigger_fail ->
    // ABORTING -> (no work in flight) -> put_submit_cleanup; the RunLoop
    // inside DispatchChunk drives put_cleanup_done, which lands in DRAINING
    // since no final chunk has arrived yet.
    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest_a, 10U, 0U, false), 50U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);
    DispatchChunk(50U, std::vector<uint8_t>(16U, 'x'), /*last=*/false);
    ASSERT_EQ(Frames().size(), 2U);
    ASSERT_EQ(ActiveStreamCount(), 1U); // A: DRAINING, still active.

    // Stream B: added second (array index 1, the entry a swap-remove of
    // index 0 would move into index 0). A live upload with a real temp file
    // on disk and bytes already written, never finished.
    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest_b, chunk_size * 2U, 0U, false),
                    51U);
    ASSERT_EQ(Frames().size(), 3U);
    ExpectAck(Frames()[2]);
    DispatchChunk(51U, std::vector<uint8_t>(chunk_size, 'a'), /*last=*/false);
    ASSERT_EQ(Frames().size(), 3U);
    EXPECT_TRUE(PathExists(temp_b_path));
    ASSERT_EQ(ActiveStreamCount(), 2U); // A: DRAINING, B: RECEIVING.

    CloseConnectionAndCancelAll();
    RunLoop();

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_FALSE(PathExists(temp_b_path));
    EXPECT_FALSE(PathExists(dest_b));
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, PutShutdownRequestedMidUploadCleansUpAndStopsLoop)
{
    const std::string dest = JoinDir("shutdownmid.txt");
    const size_t chunk_size = static_cast<size_t>(64U) * 1024U;
    char *temp = taz_fsutil_temp_name(dest.c_str(), 25U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, chunk_size * 2U, 0U, false), 25U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    DispatchChunkNoRun(25U, std::vector<uint8_t>(chunk_size, 'a'),
                       /*last=*/false);
    RequestShutdown();
    RunLoop();

    EXPECT_EQ(Frames().size(), 1U);
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());

    taz_work_reset_for_tests();
}

// ---------------------------------------------------------------------------
// CANCEL (handlers/cancel.c) targeting a FILE_PUT stream.
// ---------------------------------------------------------------------------

TEST_F(FileHandlerTest,
       CancelAfterTwoOfFourChunksArrivesOnceTempGoneThenPutDrains)
{
    const std::string dest = JoinDir("cancelme.txt");
    char *temp = taz_fsutil_temp_name(dest.c_str(), 30U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 16U, 0U, false), 30U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    DispatchChunk(30U, ToBytes("aaaa"), /*last=*/false);
    DispatchChunk(30U, ToBytes("bbbb"), /*last=*/false);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_TRUE(PathExists(temp_path));

    // The CANCEL is on its own fresh stream (31); the loop run inside
    // DispatchRequest drives the PUT's cleanup step to completion (closing
    // the fd and unlinking the temp) before on_cancelled fires, so by the
    // time cancelled=true is observed the temp is already gone.
    DispatchRequest(taz_v1_Opcode_OPCODE_CANCEL, encode_cancel_request(30U),
                    31U);
    ASSERT_EQ(Frames().size(), 2U);
    ExpectCancelled(Frames()[1], true);
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_FALSE(PathExists(dest));
    // The CANCEL stream closed; the PUT stream is still active (DRAINING)
    // until its final chunk arrives.
    EXPECT_EQ(ActiveStreamCount(), 1U);

    // A further non-final chunk on the now-DRAINING PUT stream is dropped.
    DispatchChunk(30U, ToBytes("cccc"), /*last=*/false);
    EXPECT_EQ(Frames().size(), 2U);
    EXPECT_EQ(ActiveStreamCount(), 1U);

    // The final chunk releases the PUT stream.
    DispatchChunk(30U, ToBytes("dddd"), /*last=*/true);
    EXPECT_EQ(Frames().size(), 2U);
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, CancelOnDrainingPutStreamReturnsFalse)
{
    const std::string dest = JoinDir("alreadydraining.txt");
    char *temp = taz_fsutil_temp_name(dest.c_str(), 32U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 10U, 0U, false), 32U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    // Announce 10, send 16: put_trigger_fail drives this straight to
    // DRAINING (no cancel involved) before any CANCEL is ever dispatched.
    DispatchChunk(32U, std::vector<uint8_t>(16U, 'x'), /*last=*/false);
    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_FALSE(PathExists(temp_path));
    ASSERT_EQ(ActiveStreamCount(), 1U);

    DispatchRequest(taz_v1_Opcode_OPCODE_CANCEL, encode_cancel_request(32U),
                    33U);
    ASSERT_EQ(Frames().size(), 3U);
    ExpectCancelled(Frames()[2], false);
    ASSERT_EQ(ActiveStreamCount(), 1U); // The PUT stream is still DRAINING.

    // The final chunk releases it.
    DispatchChunk(32U, {}, /*last=*/true);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest, CancelSameTargetTwiceInARowReturnsTrueThenFalse)
{
    const std::string dest = JoinDir("twice.txt");
    char *temp = taz_fsutil_temp_name(dest.c_str(), 34U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, 4U, 0U, false), 34U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    DispatchRequest(taz_v1_Opcode_OPCODE_CANCEL, encode_cancel_request(34U),
                    35U);
    ASSERT_EQ(Frames().size(), 2U);
    ExpectCancelled(Frames()[1], true);
    EXPECT_FALSE(PathExists(temp_path));
    ASSERT_EQ(ActiveStreamCount(), 1U); // PUT: DRAINING.

    // Same target again, now DRAINING: refused synchronously.
    DispatchRequest(taz_v1_Opcode_OPCODE_CANCEL, encode_cancel_request(34U),
                    36U);
    ASSERT_EQ(Frames().size(), 3U);
    ExpectCancelled(Frames()[2], false);
    ASSERT_EQ(ActiveStreamCount(), 1U);

    // Drain the PUT stream so nothing is left dangling at TearDown.
    DispatchChunk(34U, {}, /*last=*/true);
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

TEST_F(FileHandlerTest,
       CancelWhileConnectionClosingWritesNothingAndReleasesBothStreams)
{
    const std::string dest = JoinDir("cancelclose.txt");
    const size_t chunk_size = static_cast<size_t>(64U) * 1024U;
    char *temp = taz_fsutil_temp_name(dest.c_str(), 37U);
    ASSERT_NE(temp, nullptr);
    const std::string temp_path(temp);
    free(temp);

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, chunk_size * 2U, 0U, false), 37U);
    ASSERT_EQ(Frames().size(), 1U);
    ExpectAck(Frames()[0]);

    DispatchChunk(37U, std::vector<uint8_t>(chunk_size, 'a'), /*last=*/false);
    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_TRUE(PathExists(temp_path));

    // The CANCEL is accepted synchronously (put_cancel runs on this call,
    // submitting a cleanup step to the pool) but the loop never runs before
    // the connection starts closing, so neither the CANCEL's own response
    // nor the cleanup's completion has happened yet.
    DispatchRequestNoRun(taz_v1_Opcode_OPCODE_CANCEL,
                         encode_cancel_request(37U), 38U);
    ASSERT_EQ(ActiveStreamCount(), 2U);

    CloseConnectionAndCancelAll();
    RunLoop();

    EXPECT_EQ(Frames().size(), 1U); // No CancelResponse, no PUT frame.
    EXPECT_FALSE(PathExists(temp_path));
    EXPECT_FALSE(PathExists(dest));
    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), UnrefCount());
}

#ifdef _WIN32
TEST_F(FileHandlerTest, PutWithReadOnlyPermissionsSetsReadOnlyAttribute)
{
    const std::string dest = JoinDir("readonly.txt");
    const std::string content = "x";

    DispatchRequest(taz_v1_Opcode_OPCODE_FILE_PUT,
                    encode_put_request(dest, content.size(), 0444U, false),
                    18U);
    DispatchChunk(18U, ToBytes(content), /*last=*/true);

    ASSERT_EQ(Frames().size(), 2U);
    uv_fs_t req;
    ASSERT_EQ(uv_fs_stat(nullptr, &req, dest.c_str(), nullptr), 0);
    const uint64_t mode = req.statbuf.st_mode;
    uv_fs_req_cleanup(&req);
    EXPECT_EQ(mode & 0200U, 0U);
}
#endif
