// RAII guards for the RUN_AS test seams (taz/run_as.h). Both seams are
// process-global, so every test that sets one restores it on scope exit,
// including an early return from a failed ASSERT_*. Included by
// test_run_as.cpp, test_command.cpp and test_payload.cpp.

#ifndef TAZ_TESTS_RUN_AS_TEST_SUPPORT_H
#define TAZ_TESTS_RUN_AS_TEST_SUPPORT_H

#include <string>

#include "taz/run_as.h"

// Forces taz_run_as_privileged()'s answer: 1 for the privileged rows, 0 for
// the unprivileged ones (so they hold even when the suite runs as root).
// Restores the real geteuid() answer (-1) on scope exit. A no-op on
// Windows, where RUN_AS is deferred and the answer is always 0.
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

#endif /* TAZ_TESTS_RUN_AS_TEST_SUPPORT_H */
