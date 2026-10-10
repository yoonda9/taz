// Unit tests for handlers/timeout.c: the TIMEOUT_SET (0x0041) handler and
// its registration as an async OPCODE_TABLE entry.

#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "file_test_support.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"

namespace
{

std::vector<uint8_t> encode_timeout_set_request(uint32_t timeout_ms)
{
    taz_v1_TimeoutSetRequest req = taz_v1_TimeoutSetRequest_init_zero;
    req.timeout_ms = timeout_ms;
    std::vector<uint8_t> buf(taz_v1_TimeoutSetRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_TimeoutSetRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

taz_v1_TimeoutSetResponse
decode_timeout_set_response(const std::vector<uint8_t> &frame)
{
    taz_v1_TimeoutSetResponse resp = taz_v1_TimeoutSetResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(frame);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    EXPECT_TRUE(pb_decode(&istream, taz_v1_TimeoutSetResponse_fields, &resp));
    return resp;
}

class TimeoutHandlerTest : public FileHandlerTest
{
};

} // namespace

TEST_F(TimeoutHandlerTest, OpcodeIsMarkedAsync)
{
    EXPECT_TRUE(taz_dispatch_opcode_is_async(
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_TIMEOUT_SET)));
}

TEST_F(TimeoutHandlerTest, FirstSetReturnsPreviousZero)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_TIMEOUT_SET,
                    encode_timeout_set_request(500U), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    const taz_v1_TimeoutSetResponse resp =
        decode_timeout_set_response(Frames()[0]);
    EXPECT_EQ(resp.previous_ms, 0U);

    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(TimeoutHandlerTest, SecondSetReturnsPriorValue)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_TIMEOUT_SET,
                    encode_timeout_set_request(500U), 1U);
    DispatchRequest(taz_v1_Opcode_OPCODE_TIMEOUT_SET,
                    encode_timeout_set_request(0U), 2U);

    ASSERT_EQ(Frames().size(), 2U);
    const taz_v1_TimeoutSetResponse resp =
        decode_timeout_set_response(Frames()[1]);
    EXPECT_EQ(resp.previous_ms, 500U);
}

TEST_F(TimeoutHandlerTest, EmptyPayloadSetsZeroAndIsNotAnError)
{
    DispatchRequest(taz_v1_Opcode_OPCODE_TIMEOUT_SET,
                    encode_timeout_set_request(500U), 1U);
    DispatchRequest(taz_v1_Opcode_OPCODE_TIMEOUT_SET, {}, 2U);

    ASSERT_EQ(Frames().size(), 2U);
    EXPECT_EQ(unpack_header(Frames()[1]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    const taz_v1_TimeoutSetResponse first =
        decode_timeout_set_response(Frames()[1]);
    EXPECT_EQ(first.previous_ms, 500U);

    DispatchRequest(taz_v1_Opcode_OPCODE_TIMEOUT_SET,
                    encode_timeout_set_request(1U), 3U);
    const taz_v1_TimeoutSetResponse second =
        decode_timeout_set_response(Frames()[2]);
    EXPECT_EQ(second.previous_ms, 0U);
}

TEST_F(TimeoutHandlerTest, UndecodablePayloadIsInvalidRequestAndStreamReleased)
{
    const std::vector<uint8_t> garbage = {0xFFU, 0xFFU, 0xFFU};

    DispatchRequest(taz_v1_Opcode_OPCODE_TIMEOUT_SET, garbage, 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(UnrefCount(), 0);
}
