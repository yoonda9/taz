// Unit tests for handlers/run_as.c: the RUN_AS (0x0040) handler and its
// registration as an async OPCODE_TABLE entry. No gate here runs as root,
// so every row forces the privilege answer it needs through
// taz_run_as_set_privileged_for_tests (ScopedPrivilege) rather than relying
// on the real geteuid(); the rows then hold whether or not the suite runs
// as root, and in any order.

#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <pb_decode.h>
#include <pb_encode.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include "file_test_support.h"
#include "run_as_test_support.h"
#include "taz/run_as.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"

namespace
{

std::vector<uint8_t> encode_run_as_request(const char *user)
{
    taz_v1_RunAsRequest req = taz_v1_RunAsRequest_init_zero;
    if (user != nullptr)
    {
        (void)snprintf(req.user, sizeof(req.user), "%s", user);
    }
    std::vector<uint8_t> buf(taz_v1_RunAsRequest_size);
    pb_ostream_t ostream = pb_ostream_from_buffer(buf.data(), buf.size());
    EXPECT_TRUE(pb_encode(&ostream, taz_v1_RunAsRequest_fields, &req));
    buf.resize(ostream.bytes_written);
    return buf;
}

class RunAsHandlerTest : public FileHandlerTest
{
};

// Checks what taz_run_as_reset left behind once the base fixture's own
// TearDown (file_test_support.h) has already run it.
class RunAsResetOnTeardownTest : public RunAsHandlerTest
{
  protected:
    void TearDown() override
    {
        RunAsHandlerTest::TearDown();
        EXPECT_EQ(Dispatch().run_as_active, 0);
        EXPECT_EQ(Dispatch().run_as_uid, 0U);
        EXPECT_STREQ(Dispatch().run_as_user, "");
        EXPECT_EQ(Dispatch().run_as_token, nullptr);
#ifdef _WIN32
        DWORD flags = 0;
        EXPECT_FALSE(GetHandleInformation(token_, &flags));
#endif
    }

#ifdef _WIN32
    void SetToken(HANDLE token)
    {
        token_ = token;
    }
#endif

  private:
#ifdef _WIN32
    HANDLE token_ = nullptr;
#endif
};

} // namespace

TEST(RunAsPrivilege, RealAnswerFollowsEffectiveUidAndSeamRestoresIt)
{
#ifndef _WIN32
    const int real = geteuid() == 0 ? 1 : 0;
#else
    const int real = 0;
#endif
    EXPECT_EQ(taz_run_as_privileged(), real);
    {
        const ScopedPrivilege forced(1 - real);
        EXPECT_EQ(taz_run_as_privileged(), 1 - real);
    }
    EXPECT_EQ(taz_run_as_privileged(), real);
}

// ---------------------------------------------------------------------------
// taz_run_as_reset (run_as.c): closes and clears a connection's RUN_AS
// identity, including the Windows token. H4/H9.
// ---------------------------------------------------------------------------

TEST(RunAsReset, ClosesTheTokenAndZeroesEveryField)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);

    d.run_as_active = 1;
    d.run_as_uid = 4242U;
    d.run_as_gid = 4243U;
    (void)snprintf(d.run_as_user, sizeof(d.run_as_user), "%s", "alice");
    (void)snprintf(d.run_as_home, sizeof(d.run_as_home), "%s", "/home/alice");

#ifdef _WIN32
    HANDLE token = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(token, nullptr);
    d.run_as_token = token;
#endif

    taz_run_as_reset(&d);

    EXPECT_EQ(d.run_as_active, 0);
    EXPECT_EQ(d.run_as_uid, 0U);
    EXPECT_EQ(d.run_as_gid, 0U);
    EXPECT_STREQ(d.run_as_user, "");
    EXPECT_STREQ(d.run_as_home, "");
    EXPECT_EQ(d.run_as_token, nullptr);
#ifdef _WIN32
    DWORD flags = 0;
    EXPECT_FALSE(GetHandleInformation(token, &flags));
#endif
}

TEST(RunAsReset, IsIdempotentAndNullSafe)
{
    taz_dispatch_t d;
    taz_dispatch_init(&d);

    taz_run_as_reset(&d);
    taz_run_as_reset(&d);

    EXPECT_EQ(d.run_as_active, 0);
    EXPECT_EQ(d.run_as_token, nullptr);
}

TEST_F(RunAsResetOnTeardownTest, FixtureTeardownClosesAnOutstandingToken)
{
    // Seeded as a prior RUN_AS (or, once a real logon lands, as_user) would
    // have left it; this fixture's TearDown (file_test_support.h) calls
    // taz_run_as_reset itself now, so the identity - and the token - must
    // not outlive the test even though nothing in the test body resets it.
    Dispatch().run_as_active = 1;
    Dispatch().run_as_uid = 4242U;
    (void)snprintf(Dispatch().run_as_user, sizeof(Dispatch().run_as_user), "%s",
                   "alice");
#ifdef _WIN32
    HANDLE token = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(token, nullptr);
    SetToken(token);
    Dispatch().run_as_token = token;
#endif
}

TEST_F(RunAsHandlerTest, OpcodeIsMarkedAsync)
{
    EXPECT_TRUE(taz_dispatch_opcode_is_async(
        static_cast<uint16_t>(taz_v1_Opcode_OPCODE_RUN_AS)));
}

TEST_F(RunAsHandlerTest, NotPrivilegedResetRequestReturnsNotSupported)
{
    const ScopedPrivilege unprivileged(0);
    DispatchRequest(taz_v1_Opcode_OPCODE_RUN_AS, encode_run_as_request(""), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);

    EXPECT_EQ(ActiveStreamCount(), 0U);
    EXPECT_EQ(RefCount(), 0);
    EXPECT_EQ(UnrefCount(), 0);
}

TEST_F(RunAsHandlerTest, NotPrivilegedNamedUserReturnsNotSupported)
{
    // The privilege check runs before the empty-vs-named-user branch, so a
    // concrete username is refused the same way an empty one is - defence
    // in depth, same answer the client already gets locally from an
    // unadvertised opcode.
    const ScopedPrivilege unprivileged(0);
    DispatchRequest(taz_v1_Opcode_OPCODE_RUN_AS, encode_run_as_request("alice"),
                    1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED);

    EXPECT_EQ(ActiveStreamCount(), 0U);
}

TEST_F(RunAsHandlerTest, UndecodablePayloadIsInvalidRequestAndStreamReleased)
{
    const std::vector<uint8_t> garbage = {0xFFU, 0xFFU, 0xFFU};

    DispatchRequest(taz_v1_Opcode_OPCODE_RUN_AS, garbage, 1U);

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

// ---------------------------------------------------------------------------
// Privileged rows (fake-privilege seam): user lookup, storage on the
// connection, reset with "", and NOT_FOUND for an unknown user.
// taz_run_as_set_privileged_for_tests is a documented no-op on _WIN32 (RUN_AS
// is deferred there regardless, so taz_run_as_privileged stays hard-0), so
// these rows cannot run there - every request below would see NOT_SUPPORTED
// instead of the privileged behaviour under test.
// ---------------------------------------------------------------------------

#ifndef _WIN32

TEST_F(RunAsHandlerTest, PrivilegedNamedUserStoresIdentityAndRespondsSuccess)
{
    const ScopedPrivilege privileged(1);
    const std::string passwd_path = JoinDir("passwd");
    WriteFile(passwd_path, "alice:x:4242:4243:Alice:/home/alice:/bin/bash\n");
    const ScopedPasswdPath passwd(passwd_path);

    DispatchRequest(taz_v1_Opcode_OPCODE_RUN_AS, encode_run_as_request("alice"),
                    1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    taz_v1_RunAsResponse resp = taz_v1_RunAsResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_RunAsResponse_fields, &resp));
    EXPECT_TRUE(resp.success);
    EXPECT_STREQ(resp.effective_user, "alice");

    EXPECT_EQ(Dispatch().run_as_active, 1);
    EXPECT_EQ(Dispatch().run_as_uid, 4242U);
    EXPECT_EQ(Dispatch().run_as_gid, 4243U);
    EXPECT_STREQ(Dispatch().run_as_user, "alice");
    EXPECT_STREQ(Dispatch().run_as_home, "/home/alice");
}

TEST_F(RunAsHandlerTest, PrivilegedUnknownUserReturnsNotFound)
{
    const ScopedPrivilege privileged(1);
    const std::string passwd_path = JoinDir("passwd");
    WriteFile(passwd_path, "alice:x:4242:4243:Alice:/home/alice:/bin/bash\n");
    const ScopedPasswdPath passwd(passwd_path);

    DispatchRequest(taz_v1_Opcode_OPCODE_RUN_AS, encode_run_as_request("ghost"),
                    1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_ERROR));
    taz_v1_ErrorInfo err = taz_v1_ErrorInfo_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_ErrorInfo_fields, &err));
    EXPECT_EQ(err.code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);

    // A lookup miss must not leave a stale identity on the connection.
    EXPECT_EQ(Dispatch().run_as_active, 0);
}

TEST_F(RunAsHandlerTest, PrivilegedEmptyUserResetsActiveIdentity)
{
    const ScopedPrivilege privileged(1);
    const std::string passwd_path = JoinDir("passwd");
    WriteFile(passwd_path,
              "daemon-self:x:" + std::to_string(geteuid()) + ":0::/:/bin/sh\n");
    const ScopedPasswdPath passwd(passwd_path);

    // Seed an already-active identity, as a prior successful RUN_AS would
    // have left it, so this row proves the empty-user request actually
    // clears it rather than merely never having set it.
    Dispatch().run_as_active = 1;
    Dispatch().run_as_uid = 4242U;
    Dispatch().run_as_gid = 4243U;
    (void)snprintf(Dispatch().run_as_user, sizeof(Dispatch().run_as_user), "%s",
                   "alice");
    (void)snprintf(Dispatch().run_as_home, sizeof(Dispatch().run_as_home), "%s",
                   "/home/alice");

    DispatchRequest(taz_v1_Opcode_OPCODE_RUN_AS, encode_run_as_request(""), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    EXPECT_EQ(unpack_header(Frames()[0]).type,
              static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_RESPONSE));
    taz_v1_RunAsResponse resp = taz_v1_RunAsResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_RunAsResponse_fields, &resp));
    EXPECT_TRUE(resp.success);
    // "The identity now in effect" after a reset is the daemon's own.
    EXPECT_STREQ(resp.effective_user, "daemon-self");

    EXPECT_EQ(Dispatch().run_as_active, 0);
    EXPECT_EQ(Dispatch().run_as_uid, 0U);
    EXPECT_EQ(Dispatch().run_as_gid, 0U);
    EXPECT_STREQ(Dispatch().run_as_user, "");
    EXPECT_STREQ(Dispatch().run_as_home, "");
}

TEST_F(RunAsHandlerTest, PrivilegedResetReportsDecimalUidWhenDaemonHasNoEntry)
{
    const ScopedPrivilege privileged(1);
    const std::string passwd_path = JoinDir("passwd");
    WriteFile(passwd_path, "");
    const ScopedPasswdPath passwd(passwd_path);

    DispatchRequest(taz_v1_Opcode_OPCODE_RUN_AS, encode_run_as_request(""), 1U);

    ASSERT_EQ(Frames().size(), 1U);
    taz_v1_RunAsResponse resp = taz_v1_RunAsResponse_init_zero;
    const std::vector<uint8_t> body = frame_payload(Frames()[0]);
    pb_istream_t istream = pb_istream_from_buffer(body.data(), body.size());
    ASSERT_TRUE(pb_decode(&istream, taz_v1_RunAsResponse_fields, &resp));
    EXPECT_EQ(std::string(resp.effective_user), std::to_string(geteuid()));
}

#endif /* !_WIN32 */
