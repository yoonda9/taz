// RAII guards for the RUN_AS test seams (taz/run_as.h). Both seams are
// process-global, so every test that sets one restores it on scope exit,
// including an early return from a failed ASSERT_*. Included by
// test_run_as.cpp, test_command.cpp and test_payload.cpp.

#ifndef TAZ_TESTS_RUN_AS_TEST_SUPPORT_H
#define TAZ_TESTS_RUN_AS_TEST_SUPPORT_H

#include <cstdio>
#include <string>

#include "taz/run_as.h"

#ifdef _WIN32
#include "taz/run_as_win32.h"
#endif

// Forces taz_run_as_privileged()'s answer: 1 for the privileged rows, 0 for
// the unprivileged ones (so they hold even when the suite runs as root, or,
// on Windows, as SYSTEM). Restores the real answer (-1) on scope exit:
// geteuid() == 0 on POSIX, the cached SeTcbPrivilege probe on Windows.
class ScopedPrivilege
{
  public:
    explicit ScopedPrivilege(int privileged)
    {
        taz_run_as_set_privileged_for_tests(privileged);
    }
    ~ScopedPrivilege()
    {
        taz_run_as_set_privileged_for_tests(-1);
    }
    ScopedPrivilege(const ScopedPrivilege &) = delete;
    ScopedPrivilege &operator=(const ScopedPrivilege &) = delete;
};

// Points RUN_AS's user lookup at a scratch passwd file; restores the
// platform default on scope exit.
class ScopedPasswdPath
{
  public:
    explicit ScopedPasswdPath(const std::string &path)
    {
        taz_run_as_set_passwd_path_for_tests(path.c_str());
    }
    ~ScopedPasswdPath()
    {
        taz_run_as_set_passwd_path_for_tests(nullptr);
    }
    ScopedPasswdPath(const ScopedPasswdPath &) = delete;
    ScopedPasswdPath &operator=(const ScopedPasswdPath &) = delete;
};

#ifdef _WIN32

// Function-local statics in an inline function, not plain globals, so this
// header can be included from more than one test .cpp without an ODR
// violation (every TU's copy of an inline function's local static is
// guaranteed to be the one entity per [basic.def.odr]).
namespace taz_run_as_test_detail
{

inline HANDLE &LogonToken()
{
    static HANDLE token = nullptr;
    return token;
}

inline bool &LogonSucceeds()
{
    static bool succeeds = true;
    return succeeds;
}

inline taz_v1_ErrorCode &LogonCode()
{
    static taz_v1_ErrorCode code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    return code;
}

inline std::string &LogonMessage()
{
    static std::string message;
    return message;
}

inline int LogonFn(const char *, void **token, taz_v1_ErrorCode *code,
                   char *message, size_t message_size)
{
    if (LogonSucceeds())
    {
        *token = taz_run_as_token_dup(LogonToken(), nullptr, 0);
        return (*token != nullptr) ? 0 : -1;
    }
    *code = LogonCode();
    if ((message != nullptr) && (message_size > 0U))
    {
        (void)snprintf(message, message_size, "%s", LogonMessage().c_str());
    }
    return -1;
}

} // namespace taz_run_as_test_detail

// Replaces taz_run_as_win32_logon wholesale: every call under
// this scope either hands out a fresh duplicate of a fixed token or fails
// with a fixed code/message, without touching LSA. Restores the real
// logon on scope exit.
class ScopedLogon
{
  public:
    // Every logon under this scope succeeds with a duplicate of token.
    explicit ScopedLogon(HANDLE token)
    {
        taz_run_as_test_detail::LogonSucceeds() = true;
        taz_run_as_test_detail::LogonToken() = token;
        taz_run_as_win32_set_logon_for_tests(&taz_run_as_test_detail::LogonFn);
    }
    // Every logon under this scope fails with code/message.
    ScopedLogon(taz_v1_ErrorCode code, const std::string &message)
    {
        taz_run_as_test_detail::LogonSucceeds() = false;
        taz_run_as_test_detail::LogonCode() = code;
        taz_run_as_test_detail::LogonMessage() = message;
        taz_run_as_win32_set_logon_for_tests(&taz_run_as_test_detail::LogonFn);
    }
    ~ScopedLogon()
    {
        taz_run_as_win32_set_logon_for_tests(nullptr);
    }
    ScopedLogon(const ScopedLogon &) = delete;
    ScopedLogon &operator=(const ScopedLogon &) = delete;
};

#endif /* _WIN32 */

#endif /* TAZ_TESTS_RUN_AS_TEST_SUPPORT_H */
