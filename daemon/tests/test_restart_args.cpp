// Tests for the RESTART re-exec argv builder and exepath fixup (pure, no
// I/O) and the POSIX re-exec failure path.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "taz/platform.h"
#include "taz/restart_args.h"

#ifndef _WIN32
#include <cerrno>
#endif

namespace
{

// Builds a char *const argv[] (argv[0] included, NULL-terminated) from a
// list of strings, keeping the backing storage alive for the caller.
class Argv
{
  public:
    explicit Argv(std::vector<std::string> tokens) : tokens_(std::move(tokens))
    {
        for (auto &t : tokens_)
        {
            ptrs_.push_back(t.data());
        }
        ptrs_.push_back(nullptr);
    }

    char *const *data()
    {
        return ptrs_.data();
    }
    int argc() const
    {
        return static_cast<int>(tokens_.size());
    }

  private:
    std::vector<std::string> tokens_;
    std::vector<char *> ptrs_;
};

std::vector<std::string> built_to_vector(char **argv)
{
    std::vector<std::string> out;
    for (size_t i = 0; argv[i] != nullptr; i++)
    {
        out.emplace_back(argv[i]);
    }
    return out;
}

} // namespace

TEST(RestartArgs, SubstitutesExplicitPortZero)
{
    // tazd --port 0 -> <exepath> --port <bound>
    Argv original({"tazd", "--port", "0"});
    char **built = nullptr;

    ASSERT_EQ(taz_restart_args_build(original.argc(), original.data(),
                                     "/opt/tazd", 5555, &built),
              0);
    EXPECT_EQ(built_to_vector(built),
              (std::vector<std::string>{"/opt/tazd", "--port", "5555"}));
    taz_restart_args_free(built);
}

TEST(RestartArgs, AppendsPortWhenAbsent)
{
    // tazd (no --port) -> <exepath> --port <bound> (appended)
    Argv original({"tazd"});
    char **built = nullptr;

    ASSERT_EQ(taz_restart_args_build(original.argc(), original.data(),
                                     "/opt/tazd", 6000, &built),
              0);
    EXPECT_EQ(built_to_vector(built),
              (std::vector<std::string>{"/opt/tazd", "--port", "6000"}));
    taz_restart_args_free(built);
}

TEST(RestartArgs, LeavesAnExplicitPortEqualToBoundUnchanged)
{
    // tazd --port 5555 -> <exepath> --port 5555 (unchanged; equals bound)
    Argv original({"tazd", "--port", "5555"});
    char **built = nullptr;

    ASSERT_EQ(taz_restart_args_build(original.argc(), original.data(),
                                     "/opt/tazd", 5555, &built),
              0);
    EXPECT_EQ(built_to_vector(built),
              (std::vector<std::string>{"/opt/tazd", "--port", "5555"}));
    taz_restart_args_free(built);
}

TEST(RestartArgs, KeepsArgumentOrderAroundPort)
{
    // tazd --host ::1 --port 0 -> <exepath> --host ::1 --port <bound>
    Argv original({"tazd", "--host", "::1", "--port", "0"});
    char **built = nullptr;

    ASSERT_EQ(taz_restart_args_build(original.argc(), original.data(),
                                     "/opt/tazd", 7777, &built),
              0);
    EXPECT_EQ(built_to_vector(built),
              (std::vector<std::string>{"/opt/tazd", "--host", "::1", "--port",
                                        "7777"}));
    taz_restart_args_free(built);
}

TEST(RestartArgs, RejectsADanglingTrailingPortFlag)
{
    // --port is the last token (invalid; main would have rejected it) ->
    // builder returns an error, nothing built.
    Argv original({"tazd", "--host", "::1", "--port"});
    char **built = nullptr;

    EXPECT_EQ(taz_restart_args_build(original.argc(), original.data(),
                                     "/opt/tazd", 1234, &built),
              -1);
    EXPECT_EQ(built, nullptr);
}

TEST(RestartArgs, FreeToleratesNullptr)
{
    taz_restart_args_free(nullptr);
}

TEST(RestartArgs, StripsTheDeletedSuffixOfAReplacedBinary)
{
    // What Linux's /proc/self/exe reads once the running binary has been
    // replaced on disk by a rename (an install or upgrade).
    std::string path = "/opt/taz/bin/tazd (deleted)";
    taz_restart_exepath_strip_deleted(path.data());
    EXPECT_STREQ(path.c_str(), "/opt/taz/bin/tazd");
}

TEST(RestartArgs, LeavesAnOrdinaryExepathAlone)
{
    for (const char *original :
         {"/opt/taz/bin/tazd", "/opt/taz/bin/tazd (deleted) copy", " (deleted)",
          ""})
    {
        std::string path = original;
        taz_restart_exepath_strip_deleted(path.data());
        EXPECT_STREQ(path.c_str(), original);
    }
}

#ifndef _WIN32
TEST(RestartArgs, ReexecOfAMissingExecutableFailsWithoutCrashing)
{
    // POSIX execv fails (e.g. exepath unlinked) -> this function
    // returns -1 instead of replacing the process image.
    Argv missing({"/nonexistent/path/to/tazd", "--port", "5555"});

    errno = 0;
    EXPECT_EQ(taz_platform_reexec(missing.data()), -1);
    EXPECT_EQ(errno, ENOENT);
}
#endif
