// Windows-only helpers for tests that need a real, usable primary token.
// No second account exists on the host yet, so these rows duplicate the
// test process's own token instead, enabling the handful of privileges a
// later CreateProcessAsUserW/LoadUserProfileW spawn needs.
// Included only from Windows-only test code (#ifdef _WIN32 in test_exec.cpp).

#ifndef TAZ_TESTS_WIN32_TOKEN_TEST_SUPPORT_H
#define TAZ_TESTS_WIN32_TOKEN_TEST_SUPPORT_H

#include <string>

#include <windows.h>

// Closes a HANDLE on scope exit. A default-constructed or moved-from
// instance holds nullptr, treated the same as INVALID_HANDLE_VALUE: nothing
// to close.
class ScopedHandle
{
  public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) : handle_(h)
    {
    }
    ~ScopedHandle()
    {
        Reset();
    }
    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;
    ScopedHandle(ScopedHandle &&other) noexcept : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }
    ScopedHandle &operator=(ScopedHandle &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HANDLE get() const
    {
        return handle_;
    }
    bool valid() const
    {
        return (handle_ != nullptr) && (handle_ != INVALID_HANDLE_VALUE);
    }
    void Reset(HANDLE h = nullptr)
    {
        if (valid())
        {
            (void)CloseHandle(handle_);
        }
        handle_ = h;
    }
    // Gives up ownership without closing - for handing the handle to a
    // callee (e.g. taz_exec_start) that takes ownership of it itself.
    HANDLE release()
    {
        HANDLE h = handle_;
        handle_ = nullptr;
        return h;
    }

  private:
    HANDLE handle_ = nullptr;
};

namespace
{

struct PrivilegeSpec
{
    LPCWSTR name;
    const char *label;
};

bool EnablePrivilege(HANDLE token, const PrivilegeSpec &spec,
                     std::string *reason)
{
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, spec.name, &luid))
    {
        *reason = std::string(spec.label) + ": LookupPrivilegeValueW failed";
        return false;
    }

    TOKEN_PRIVILEGES priv{};
    priv.PrivilegeCount = 1;
    priv.Privileges[0].Luid = luid;
    priv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    SetLastError(0);
    const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &priv,
                                                sizeof(priv), nullptr, nullptr);
    if (!adjusted || (GetLastError() == ERROR_NOT_ALL_ASSIGNED))
    {
        *reason = std::string(spec.label) + ": not held by this session";
        return false;
    }
    return true;
}

} // namespace

// A usable primary token duplicated from the test process's own token, with
// SeIncreaseQuotaPrivilege/SeBackupPrivilege/SeRestorePrivilege enabled -
// what CreateProcessAsUserW/LoadUserProfileW need. Returns an invalid handle
// and fills *reason with the missing privilege or failing call when the
// current session cannot provide one; callers GTEST_SKIP() with it rather
// than failing the whole run, since this is an environment constraint, not
// a code bug.
inline ScopedHandle OwnPrimaryToken(std::string *reason)
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY | TOKEN_DUPLICATE, &raw))
    {
        *reason = "OpenProcessToken failed";
        return ScopedHandle();
    }
    ScopedHandle token(raw);

    // Not SE_INCREASE_QUOTA_NAME & co.: those expand through TEXT(), which
    // is the narrow (const char *) form unless UNICODE/_UNICODE is defined
    // for this translation unit, but LookupPrivilegeValueW always wants a
    // wide string.
    static const PrivilegeSpec kPrivileges[] = {
        {L"SeIncreaseQuotaPrivilege", "SeIncreaseQuotaPrivilege"},
        {L"SeBackupPrivilege", "SeBackupPrivilege"},
        {L"SeRestorePrivilege", "SeRestorePrivilege"},
    };
    for (const auto &spec : kPrivileges)
    {
        if (!EnablePrivilege(token.get(), spec, reason))
        {
            return ScopedHandle();
        }
    }

    HANDLE dup = nullptr;
    if (!DuplicateTokenEx(token.get(), TOKEN_ALL_ACCESS, nullptr,
                          SecurityImpersonation, TokenPrimary, &dup))
    {
        *reason = "DuplicateTokenEx failed";
        return ScopedHandle();
    }
    return ScopedHandle(dup);
}

#endif /* TAZ_TESTS_WIN32_TOKEN_TEST_SUPPORT_H */
