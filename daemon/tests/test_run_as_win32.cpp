// Unit tests for taz/run_as_win32.h (daemon/src/platform/run_as_win32.c):
// the S4U submit-buffer layouts (table L1/L2), the status/error mapping
// tables (L4-L7), the privilege probe (P1) and daemon-user lookup (P5) on
// the admin token every Windows gate actually has, two real LsaLogonUser
// calls proving table L11's "no crash, defined error" and table L7's
// identification-level failure, and the logon test seam (L12). Windows-
// only: the whole file compiles out elsewhere, the pattern
// test_process_linux.cpp uses for a platform-only test TU.
#ifdef _WIN32

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// Same WIN32_NO_STATUS / <ntstatus.h> trick run_as_win32.c uses, so this
// file sees the same full NTSTATUS set its own table L4-L6 rows exercise.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntsecapi.h>
#include <ntstatus.h>

#include "run_as_test_support.h"
#include "taz/run_as.h"
#include "taz/run_as_win32.h"
#include "win32_token_test_support.h"

namespace
{

// Walks the token's own TOKEN_PRIVILEGES looking for name - true only when
// the privilege is actually assigned to the token (enabled or not), never
// just "would be nice to have". Used to prove *why* the real probe answers
// 0 on the admin token (SeTcbPrivilege absent), not just that it does.
bool TokenHasPrivilege(HANDLE token, LPCWSTR name)
{
    LUID luid;
    if (LookupPrivilegeValueW(nullptr, name, &luid) == 0)
    {
        return false;
    }

    DWORD size = 0;
    (void)GetTokenInformation(token, TokenPrivileges, nullptr, 0, &size);
    if (size == 0)
    {
        return false;
    }
    std::vector<unsigned char> buf(size);
    if (GetTokenInformation(token, TokenPrivileges, buf.data(), size, &size) ==
        0)
    {
        return false;
    }

    const auto *privs = reinterpret_cast<const TOKEN_PRIVILEGES *>(buf.data());
    for (DWORD i = 0; i < privs->PrivilegeCount; i++)
    {
        const LUID &have = privs->Privileges[i].Luid;
        if ((have.LowPart == luid.LowPart) && (have.HighPart == luid.HighPart))
        {
            return true;
        }
    }
    return false;
}

std::string CurrentUserNameUtf8()
{
    WCHAR wide[256];
    DWORD len = static_cast<DWORD>(sizeof(wide) / sizeof(wide[0]));
    if (GetUserNameW(wide, &len) == 0)
    {
        return "";
    }
    char utf8[256];
    const int mbc =
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8,
                            static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    return (mbc > 0) ? std::string(utf8) : std::string();
}

} // namespace

// ---------------------------------------------------------------------------
// L1/L2 - S4U submit buffer layouts
// ---------------------------------------------------------------------------

TEST(RunAsWin32MsvS4uLayout, StructAtOffsetZeroWithStringsInsideBuffer)
{
    const wchar_t *const user = L"alice";
    const wchar_t *const domain = L"WIN-9200";
    std::vector<unsigned char> buf(256, 0xCDU);

    const size_t need =
        taz_run_as_win32_msv_s4u_layout(user, domain, buf.data(), buf.size());

    const size_t user_bytes = wcslen(user) * sizeof(wchar_t);
    const size_t domain_bytes = wcslen(domain) * sizeof(wchar_t);
    ASSERT_EQ(need, sizeof(MSV1_0_S4U_LOGON) + user_bytes + domain_bytes);

    const auto *msg = reinterpret_cast<const MSV1_0_S4U_LOGON *>(buf.data());
    EXPECT_EQ(msg->MessageType, MsV1_0S4ULogon);
    EXPECT_EQ(msg->Flags, 0UL);

    const auto *user_ptr =
        reinterpret_cast<const unsigned char *>(msg->UserPrincipalName.Buffer);
    EXPECT_EQ(user_ptr, buf.data() + sizeof(MSV1_0_S4U_LOGON));
    EXPECT_EQ(msg->UserPrincipalName.Length, user_bytes);
    EXPECT_EQ(msg->UserPrincipalName.MaximumLength, user_bytes);
    EXPECT_EQ(0, memcmp(user_ptr, user, user_bytes));

    const auto *domain_ptr =
        reinterpret_cast<const unsigned char *>(msg->DomainName.Buffer);
    EXPECT_EQ(domain_ptr, user_ptr + user_bytes);
    EXPECT_EQ(msg->DomainName.Length, domain_bytes);
    EXPECT_EQ(msg->DomainName.MaximumLength, domain_bytes);
    EXPECT_EQ(0, memcmp(domain_ptr, domain, domain_bytes));
}

TEST(RunAsWin32MsvS4uLayout, TooSmallBufferReturnsZeroAndIsUntouched)
{
    unsigned char buf[4] = {1, 2, 3, 4};

    EXPECT_EQ(taz_run_as_win32_msv_s4u_layout(L"alice", L"WIN-9200", buf,
                                              sizeof(buf)),
              0U);
    EXPECT_EQ(buf[0], 1);
    EXPECT_EQ(buf[3], 4);
}

TEST(RunAsWin32KerbS4uLayout, StructAtOffsetZeroWithUpnInsideBufferRealmEmpty)
{
    const wchar_t *const upn = L"alice@corp.example";
    std::vector<unsigned char> buf(256, 0xCDU);

    const size_t need =
        taz_run_as_win32_kerb_s4u_layout(upn, buf.data(), buf.size());

    const size_t upn_bytes = wcslen(upn) * sizeof(wchar_t);
    ASSERT_EQ(need, sizeof(KERB_S4U_LOGON) + upn_bytes);

    const auto *msg = reinterpret_cast<const KERB_S4U_LOGON *>(buf.data());
    EXPECT_EQ(msg->MessageType, KerbS4ULogon);
    EXPECT_EQ(msg->Flags, 0UL);

    const auto *upn_ptr =
        reinterpret_cast<const unsigned char *>(msg->ClientUpn.Buffer);
    EXPECT_EQ(upn_ptr, buf.data() + sizeof(KERB_S4U_LOGON));
    EXPECT_EQ(msg->ClientUpn.Length, upn_bytes);
    EXPECT_EQ(msg->ClientUpn.MaximumLength, upn_bytes);
    EXPECT_EQ(0, memcmp(upn_ptr, upn, upn_bytes));

    EXPECT_EQ(msg->ClientRealm.Length, 0);
    EXPECT_EQ(msg->ClientRealm.MaximumLength, 0);
}

TEST(RunAsWin32KerbS4uLayout, TooSmallBufferReturnsZero)
{
    unsigned char buf[4];

    EXPECT_EQ(taz_run_as_win32_kerb_s4u_layout(L"alice@corp.example", buf,
                                               sizeof(buf)),
              0U);
}

// ---------------------------------------------------------------------------
// L4-L6 - NTSTATUS mapping
// ---------------------------------------------------------------------------

TEST(RunAsWin32MapStatus, NotFoundStatuses)
{
    const NTSTATUS statuses[] = {STATUS_NO_SUCH_USER, STATUS_NO_SUCH_DOMAIN,
                                 STATUS_LOGON_FAILURE,
                                 STATUS_NO_TRUST_SAM_ACCOUNT};
    for (const NTSTATUS s : statuses)
    {
        EXPECT_EQ(taz_run_as_win32_map_status(static_cast<unsigned long>(s)),
                  taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
    }
}

TEST(RunAsWin32MapStatus, PermissionDeniedStatuses)
{
    const NTSTATUS statuses[] = {
        STATUS_LOGON_TYPE_NOT_GRANTED, STATUS_ACCOUNT_DISABLED,
        STATUS_ACCOUNT_LOCKED_OUT,     STATUS_ACCOUNT_RESTRICTION,
        STATUS_ACCOUNT_EXPIRED,        STATUS_PASSWORD_EXPIRED,
        STATUS_PASSWORD_MUST_CHANGE,   STATUS_INVALID_LOGON_HOURS,
        STATUS_INVALID_WORKSTATION,    STATUS_PRIVILEGE_NOT_HELD,
        STATUS_ACCESS_DENIED};
    for (const NTSTATUS s : statuses)
    {
        EXPECT_EQ(taz_run_as_win32_map_status(static_cast<unsigned long>(s)),
                  taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED);
    }
}

TEST(RunAsWin32MapStatus, AnyOtherStatusIsInternal)
{
    const NTSTATUS statuses[] = {STATUS_NO_LOGON_SERVERS,
                                 STATUS_TRUSTED_DOMAIN_FAILURE};
    for (const NTSTATUS s : statuses)
    {
        EXPECT_EQ(taz_run_as_win32_map_status(static_cast<unsigned long>(s)),
                  taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
    }
}

// ---------------------------------------------------------------------------
// L7 - DuplicateTokenEx error mapping
// ---------------------------------------------------------------------------

TEST(RunAsWin32MapDupError, PermissionDeniedErrors)
{
    const unsigned long errors[] = {ERROR_BAD_IMPERSONATION_LEVEL,
                                    ERROR_PRIVILEGE_NOT_HELD,
                                    ERROR_ACCESS_DENIED};
    for (const unsigned long e : errors)
    {
        EXPECT_EQ(taz_run_as_win32_map_dup_error(e),
                  taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED);
    }
}

TEST(RunAsWin32MapDupError, AnyOtherErrorIsInternal)
{
    EXPECT_EQ(taz_run_as_win32_map_dup_error(ERROR_NOT_ENOUGH_MEMORY),
              taz_v1_ErrorCode_ERROR_CODE_INTERNAL);
}

// ---------------------------------------------------------------------------
// P1 - the privilege probe, on the token every Windows gate actually has
// ---------------------------------------------------------------------------

TEST(RunAsWin32Privileged, AdminTokenLacksSeTcbSoTheRealAnswerIsZero)
{
    HANDLE raw = nullptr;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw));
    const ScopedHandle token(raw);

    // The specific reason the probe answers 0: SeTcbPrivilege is not on
    // this token at all (not merely disabled) - checked directly here
    // rather than through a production-only helper.
    EXPECT_FALSE(TokenHasPrivilege(token.get(), L"SeTcbPrivilege"));

    EXPECT_EQ(taz_run_as_win32_privileged(), 0);
    // Cached (table P1): a second call gives the same answer without
    // re-probing (nothing observable distinguishes a cache hit from a
    // fresh probe here, but this at least proves repeat calls are stable).
    EXPECT_EQ(taz_run_as_win32_privileged(), 0);
}

// ---------------------------------------------------------------------------
// P5 - the daemon's own account
// ---------------------------------------------------------------------------

TEST(RunAsWin32DaemonUser, NonEmptyAndContainsADomainSeparator)
{
    char buf[256];

    taz_run_as_win32_daemon_user(buf, sizeof(buf));

    EXPECT_NE(buf[0], '\0');
    EXPECT_NE(std::string(buf).find('\\'), std::string::npos);
}

TEST(RunAsWin32DaemonUser, ZeroSizeBufferIsANoOp)
{
    char buf[1] = {'x'};

    taz_run_as_win32_daemon_user(buf, 0U);

    EXPECT_EQ(buf[0], 'x');
}

// ---------------------------------------------------------------------------
// A real LsaLogonUser, from the admin token this gate actually has (no
// SeTcbPrivilege, no domain controller reachable).
// ---------------------------------------------------------------------------

TEST(RunAsWin32Logon, UnresolvableDomainReturnsDefinedErrorAndDoesNotCrash)
{
    void *token = nullptr;
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    char message[256] = {0};

    const int rc = taz_run_as_win32_logon("NODOMAIN\\x", &token, &code, message,
                                          sizeof(message));

    // Unconditional, not just on failure: progress.md records the exact
    // status this KDC-less host returns (table L11), and the message
    // text (table L9) always carries it as "0x%08lX".
    std::cerr << "UnresolvableDomainReturnsDefinedErrorAndDoesNotCrash: "
              << message << "\n";

    EXPECT_EQ(rc, -1);
    EXPECT_EQ(token, nullptr);
    EXPECT_TRUE((code == taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND) ||
                (code == taz_v1_ErrorCode_ERROR_CODE_INTERNAL));
    EXPECT_NE(message[0], '\0');
}

TEST(RunAsWin32Logon, OwnAccountWithoutSeTcbIsPermissionDenied)
{
    const std::string user = CurrentUserNameUtf8();
    ASSERT_FALSE(user.empty());
    const std::string input = ".\\" + user;

    void *token = nullptr;
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    char message[256] = {0};

    const int rc = taz_run_as_win32_logon(input.c_str(), &token, &code, message,
                                          sizeof(message));

    std::cerr << "OwnAccountWithoutSeTcbIsPermissionDenied: " << message
              << "\n";

    EXPECT_EQ(rc, -1);
    EXPECT_EQ(token, nullptr);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED);
    EXPECT_NE(message[0], '\0');
}

TEST(RunAsWin32Logon, MalformedNameIsInvalidRequestBeforeAnyLsaCall)
{
    void *token = nullptr;
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    char message[256] = {0};

    const int rc =
        taz_run_as_win32_logon("", &token, &code, message, sizeof(message));

    EXPECT_EQ(rc, -1);
    EXPECT_EQ(token, nullptr);
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
    EXPECT_STREQ(message, "invalid user name");
}

// ---------------------------------------------------------------------------
// L12 - the logon test seam
// ---------------------------------------------------------------------------

TEST(RunAsWin32LogonSeam, ReplacesTheRealLogonWholesaleAndRestoresOnScopeExit)
{
    std::string reason;
    const ScopedHandle own = OwnPrimaryToken(&reason);
    if (!own.valid())
    {
        GTEST_SKIP() << reason;
    }

    {
        const ScopedLogon fake(own.get());
        void *token = nullptr;
        taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
        char message[64] = {0};

        const int rc = taz_run_as_win32_logon("whatever", &token, &code,
                                              message, sizeof(message));

        EXPECT_EQ(rc, 0);
        ASSERT_NE(token, nullptr);
        taz_run_as_token_close(token);
    }

    {
        const ScopedLogon fake(taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND, "boom");
        void *token = nullptr;
        taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
        char message[64] = {0};

        const int rc = taz_run_as_win32_logon("whatever", &token, &code,
                                              message, sizeof(message));

        EXPECT_EQ(rc, -1);
        EXPECT_EQ(token, nullptr);
        EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND);
        EXPECT_STREQ(message, "boom");
    }

    // The seam is NULL again outside both scopes: a real (failing, since
    // NODOMAIN has no DC either way) logon reaches real_logon, not the fake.
    void *token = nullptr;
    taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    char message[64] = {0};
    (void)taz_run_as_win32_logon("", &token, &code, message, sizeof(message));
    EXPECT_EQ(code, taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST);
}

#endif /* _WIN32 */
