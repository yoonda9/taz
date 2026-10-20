// Unit tests for run_as_name.c: taz_run_as_parse_name, the pure name
// classification shared by POSIX and Windows RUN_AS/as_user resolution.
// One TEST per accepted (or rejected) name form.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "taz/run_as.h"

namespace
{

constexpr char kComputer[] = "WIN-9200";

} // namespace

TEST(RunAsParseName, N1_BareNameIsLocalWithComputerDomain)
{
    taz_run_as_name_t out;

    ASSERT_EQ(taz_run_as_parse_name("alice", kComputer, &out), 0);
    EXPECT_EQ(out.kind, TAZ_RUN_AS_NAME_LOCAL);
    EXPECT_STREQ(out.user, "alice");
    EXPECT_STREQ(out.domain, kComputer);
}

TEST(RunAsParseName, N2_DotBackslashIsLocalWithComputerDomain)
{
    taz_run_as_name_t out;

    ASSERT_EQ(taz_run_as_parse_name(".\\alice", kComputer, &out), 0);
    EXPECT_EQ(out.kind, TAZ_RUN_AS_NAME_LOCAL);
    EXPECT_STREQ(out.user, "alice");
    EXPECT_STREQ(out.domain, kComputer);
}

TEST(RunAsParseName, N3_ComputerNameBackslashMatchesCaseInsensitively)
{
    for (const char *input : {"win-9200\\alice", "WIN-9200\\alice"})
    {
        SCOPED_TRACE(input);
        taz_run_as_name_t out;

        ASSERT_EQ(taz_run_as_parse_name(input, kComputer, &out), 0);
        EXPECT_EQ(out.kind, TAZ_RUN_AS_NAME_LOCAL);
        EXPECT_STREQ(out.user, "alice");
        EXPECT_STREQ(out.domain, kComputer);
    }
}

TEST(RunAsParseName, N4_OtherComputerBackslashIsDomainSplit)
{
    taz_run_as_name_t out;

    ASSERT_EQ(taz_run_as_parse_name("CORP\\alice", kComputer, &out), 0);
    EXPECT_EQ(out.kind, TAZ_RUN_AS_NAME_DOMAIN);
    EXPECT_STREQ(out.user, "alice");
    EXPECT_STREQ(out.domain, "CORP");
}

TEST(RunAsParseName, N5_UserPrincipalNameIsDomainVerbatim)
{
    taz_run_as_name_t out;

    ASSERT_EQ(taz_run_as_parse_name("alice@corp.example", kComputer, &out), 0);
    EXPECT_EQ(out.kind, TAZ_RUN_AS_NAME_DOMAIN);
    EXPECT_STREQ(out.user, "alice@corp.example");
    EXPECT_STREQ(out.domain, "");
}

TEST(RunAsParseName, N6_MalformedNamesAreRejected)
{
    // TAZ_EXEC_USER_MAX bytes of 'x': exactly too long to fit a part plus
    // its NUL in a TAZ_EXEC_USER_MAX-sized buffer.
    const std::string too_long(TAZ_EXEC_USER_MAX, 'x');
    const std::vector<std::string> inputs = {
        "",
        "\\alice",
        "CORP\\",
        "a\\b\\c",
        "a@b\\c",
        "@x",
        "x@",
        too_long,
        "CORP\\" + too_long,
        too_long + "@corp.example",
    };

    for (const auto &input : inputs)
    {
        SCOPED_TRACE(input);
        taz_run_as_name_t out;

        EXPECT_EQ(taz_run_as_parse_name(input.c_str(), kComputer, &out), -1);
    }
}

TEST(RunAsParseName, N7_EmptyOrNullComputerNameMakesNamedFormsDomain)
{
    taz_run_as_name_t out;

    for (const char *computer_name :
         {static_cast<const char *>(""), static_cast<const char *>(nullptr)})
    {
        SCOPED_TRACE(computer_name == nullptr ? "NULL" : computer_name);

        ASSERT_EQ(taz_run_as_parse_name("CORP\\alice", computer_name, &out), 0);
        EXPECT_EQ(out.kind, TAZ_RUN_AS_NAME_DOMAIN);
        EXPECT_STREQ(out.user, "alice");
        EXPECT_STREQ(out.domain, "CORP");

        ASSERT_EQ(taz_run_as_parse_name("alice", computer_name, &out), 0);
        EXPECT_EQ(out.kind, TAZ_RUN_AS_NAME_LOCAL);
        EXPECT_STREQ(out.user, "alice");
        EXPECT_STREQ(out.domain, "");
    }
}

TEST(RunAsBareUser, EveryAcceptedFormYieldsTheAccountName)
{
    for (const char *input : {"alice", ".\\alice", "WIN-9200\\alice",
                              "CORP\\alice", "alice@corp.example"})
    {
        SCOPED_TRACE(input);
        char buf[TAZ_EXEC_USER_MAX];

        EXPECT_STREQ(taz_run_as_bare_user(input, buf, sizeof(buf)), "alice");
    }
}

TEST(RunAsBareUser, TruncatesToTheBuffer)
{
    char buf[4];

    EXPECT_STREQ(taz_run_as_bare_user("CORP\\alice", buf, sizeof(buf)), "ali");
    EXPECT_STREQ(taz_run_as_bare_user("", buf, sizeof(buf)), "");
}
