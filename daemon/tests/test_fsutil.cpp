// Unit tests for taz/fsutil.h: pure path/mode helpers and the /etc/passwd
// uid->name parser.

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <uv.h>

#include "taz/fsutil.h"
#include "taz/v1/common.pb.h"

namespace
{

// ---------------------------------------------------------------------------
// taz_fsutil_kind_from_mode
// ---------------------------------------------------------------------------

TEST(KindFromMode, RegularFileMapsToFile)
{
    EXPECT_EQ(taz_fsutil_kind_from_mode(S_IFREG | 0644), taz_v1_Kind_KIND_FILE);
}

TEST(KindFromMode, DirectoryMapsToDir)
{
    EXPECT_EQ(taz_fsutil_kind_from_mode(S_IFDIR | 0755), taz_v1_Kind_KIND_DIR);
}

TEST(KindFromMode, SymlinkMapsToSymlink)
{
    EXPECT_EQ(taz_fsutil_kind_from_mode(S_IFLNK | 0777),
              taz_v1_Kind_KIND_SYMLINK);
}

TEST(KindFromMode, OtherMapsToOther)
{
    EXPECT_EQ(taz_fsutil_kind_from_mode(S_IFCHR), taz_v1_Kind_KIND_OTHER);
}

// ---------------------------------------------------------------------------
// taz_fsutil_is_hidden
// ---------------------------------------------------------------------------

TEST(IsHidden, LeadingDotIsHidden)
{
    EXPECT_TRUE(taz_fsutil_is_hidden(".git"));
    EXPECT_TRUE(taz_fsutil_is_hidden("."));
}

TEST(IsHidden, NoLeadingDotIsNotHidden)
{
    EXPECT_FALSE(taz_fsutil_is_hidden("file.txt"));
    EXPECT_FALSE(taz_fsutil_is_hidden(""));
}

TEST(IsHidden, NullIsNotHidden)
{
    EXPECT_FALSE(taz_fsutil_is_hidden(nullptr));
}

// ---------------------------------------------------------------------------
// taz_fsutil_join
// ---------------------------------------------------------------------------

TEST(Join, NoTrailingSeparatorAddsOne)
{
    char *out = taz_fsutil_join("dir", "name");
    ASSERT_NE(out, nullptr);
#ifdef _WIN32
    EXPECT_STREQ(out, "dir\\name");
#else
    EXPECT_STREQ(out, "dir/name");
#endif
    free(out);
}

TEST(Join, TrailingSeparatorNotDoubled)
{
#ifdef _WIN32
    char *out = taz_fsutil_join("dir\\", "name");
#else
    char *out = taz_fsutil_join("dir/", "name");
#endif
    ASSERT_NE(out, nullptr);
#ifdef _WIN32
    EXPECT_STREQ(out, "dir\\name");
#else
    EXPECT_STREQ(out, "dir/name");
#endif
    free(out);
}

TEST(Join, EmptyDirYieldsBareName)
{
    char *out = taz_fsutil_join("", "name");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "name");
    free(out);
}

// ---------------------------------------------------------------------------
// taz_fsutil_chmod_unrepresentable
// ---------------------------------------------------------------------------

TEST(ChmodUnrepresentable, CommonModesAreRepresentable)
{
    EXPECT_EQ(taz_fsutil_chmod_unrepresentable(0644U), 0U);
    EXPECT_EQ(taz_fsutil_chmod_unrepresentable(0755U), 0U);
    EXPECT_EQ(taz_fsutil_chmod_unrepresentable(0444U), 0U);
    EXPECT_EQ(taz_fsutil_chmod_unrepresentable(0666U), 0U);
}

TEST(ChmodUnrepresentable, MissingReadBitIsUnrepresentable)
{
    EXPECT_NE(taz_fsutil_chmod_unrepresentable(0600U), 0U);
    EXPECT_NE(taz_fsutil_chmod_unrepresentable(0000U), 0U);
}

TEST(ChmodUnrepresentable, SpecialBitsAreUnrepresentable)
{
    EXPECT_NE(taz_fsutil_chmod_unrepresentable(04755U), 0U);
}

TEST(ChmodUnrepresentable, GroupOtherWriteWithoutOwnerWriteIsUnrepresentable)
{
    EXPECT_NE(taz_fsutil_chmod_unrepresentable(0422U), 0U);
    // All read bits are present here, so this isolates the group/other-write
    // clause from the missing-read-bit clause above.
    EXPECT_NE(taz_fsutil_chmod_unrepresentable(0466U), 0U);
}

// ---------------------------------------------------------------------------
// taz_fsutil_is_sep
// ---------------------------------------------------------------------------

TEST(IsSep, ForwardSlashIsAlwaysASeparator)
{
    EXPECT_TRUE(taz_fsutil_is_sep('/'));
}

TEST(IsSep, RegularCharacterIsNotASeparator)
{
    EXPECT_FALSE(taz_fsutil_is_sep('a'));
}

#ifdef _WIN32
TEST(IsSep, BackslashIsASeparatorOnWindows)
{
    EXPECT_TRUE(taz_fsutil_is_sep('\\'));
}
#else
TEST(IsSep, BackslashIsNotASeparatorOnPosix)
{
    EXPECT_FALSE(taz_fsutil_is_sep('\\'));
}
#endif

// ---------------------------------------------------------------------------
// taz_fsutil_root_prefix_len
// ---------------------------------------------------------------------------

TEST(RootPrefixLen, PosixRootIsOne)
{
    EXPECT_EQ(taz_fsutil_root_prefix_len("/"), 1U);
}

TEST(RootPrefixLen, RelativePathIsZero)
{
    EXPECT_EQ(taz_fsutil_root_prefix_len("a/b"), 0U);
}

#ifdef _WIN32

TEST(RootPrefixLen, DriveWithBackslashIsThree)
{
    EXPECT_EQ(taz_fsutil_root_prefix_len("C:\\"), 3U);
}

TEST(RootPrefixLen, DriveWithForwardSlashIsThree)
{
    EXPECT_EQ(taz_fsutil_root_prefix_len("C:/"), 3U);
}

TEST(RootPrefixLen, BareDriveIsTwo)
{
    EXPECT_EQ(taz_fsutil_root_prefix_len("C:"), 2U);
}

TEST(RootPrefixLen, LongPathDriveIsSeven)
{
    EXPECT_EQ(taz_fsutil_root_prefix_len("\\\\?\\C:\\"), 7U);
}

TEST(RootPrefixLen, UncRootIsThroughShareSeparator)
{
    const char *path = "\\\\server\\share\\";
    EXPECT_EQ(taz_fsutil_root_prefix_len(path), strlen(path));
}

#endif /* _WIN32 */

// ---------------------------------------------------------------------------
// taz_passwd_name_from_uid
// ---------------------------------------------------------------------------

class PasswdNameFromUid : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        char tmpdir[1024];
        size_t tmpdir_len = sizeof(tmpdir) - 1;
        ASSERT_EQ(uv_os_tmpdir(tmpdir, &tmpdir_len), 0);
        const std::string tpl_str =
            std::string(tmpdir, tmpdir_len) + "/taz_fsutil_test_XXXXXX";
        std::vector<char> tpl(tpl_str.begin(), tpl_str.end());
        tpl.push_back('\0');

        uv_fs_t req;
        const int rc = uv_fs_mkdtemp(NULL, &req, tpl.data(), NULL);
        if (rc != 0)
        {
            uv_fs_req_cleanup(&req);
        }
        ASSERT_EQ(rc, 0);
        dir_ = req.path;
        uv_fs_req_cleanup(&req);
        path_ = dir_ + "/passwd";
    }

    void TearDown() override
    {
        uv_fs_t req;
        (void)uv_fs_unlink(NULL, &req, path_.c_str(), NULL);
        uv_fs_req_cleanup(&req);
        uv_fs_t rmdir_req;
        (void)uv_fs_rmdir(NULL, &rmdir_req, dir_.c_str(), NULL);
        uv_fs_req_cleanup(&rmdir_req);
    }

    void WriteFile(const std::string &contents) const
    {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << contents;
    }

    const std::string &Path() const
    {
        return path_;
    }

  private:
    std::string dir_;
    std::string path_;
};

TEST_F(PasswdNameFromUid, FindsMatchingUid)
{
    WriteFile("root:x:0:0:root:/root:/bin/bash\n"
              "alice:x:1000:1000:Alice:/home/alice:/bin/bash\n");
    char buf[64];
    EXPECT_EQ(
        taz_passwd_name_from_uid(Path().c_str(), 1000UL, buf, sizeof(buf)), 1);
    EXPECT_STREQ(buf, "alice");
}

TEST_F(PasswdNameFromUid, NoMatchReturnsZero)
{
    WriteFile("root:x:0:0:root:/root:/bin/bash\n");
    char buf[64];
    EXPECT_EQ(taz_passwd_name_from_uid(Path().c_str(), 999UL, buf, sizeof(buf)),
              0);
}

TEST_F(PasswdNameFromUid, MissingFileReturnsZero)
{
    char buf[64];
    EXPECT_EQ(taz_passwd_name_from_uid((Path() + "-missing").c_str(), 0UL, buf,
                                       sizeof(buf)),
              0);
}

TEST_F(PasswdNameFromUid, MalformedLinesAreSkipped)
{
    WriteFile("no-colons-at-all\n"
              "only:one-colon\n"
              "bob:x:42:42:Bob:/home/bob:/bin/bash\n");
    char buf[64];
    EXPECT_EQ(taz_passwd_name_from_uid(Path().c_str(), 42UL, buf, sizeof(buf)),
              1);
    EXPECT_STREQ(buf, "bob");
}

TEST_F(PasswdNameFromUid, EmptyUidFieldIsSkipped)
{
    WriteFile("empty:x::0:Empty:/home/empty:/bin/bash\n"
              "carol:x:3:3:Carol:/home/carol:/bin/bash\n");
    char buf[64];
    EXPECT_EQ(taz_passwd_name_from_uid(Path().c_str(), 3UL, buf, sizeof(buf)),
              1);
    EXPECT_STREQ(buf, "carol");
}

TEST_F(PasswdNameFromUid, NonNumericUidFieldIsSkipped)
{
    WriteFile("bad:x:notanumber:0:Bad:/home/bad:/bin/bash\n"
              "good:x:7:7:Good:/home/good:/bin/bash\n");
    char buf[64];
    EXPECT_EQ(taz_passwd_name_from_uid(Path().c_str(), 7UL, buf, sizeof(buf)),
              1);
    EXPECT_STREQ(buf, "good");
}

TEST_F(PasswdNameFromUid, OverlongLineIsSkippedNotMisparsed)
{
    // A line longer than the internal buffer must never have its tail
    // reparsed as a fresh "name:x:uid:..." entry. 511 'A' chars fill the
    // internal line buffer (PASSWD_LINE_MAX == 512) exactly, so the tail
    // left behind is "evil:x:1000:...", a line that would misparse as a
    // match for uid 1000 if the drain logic were missing.
    WriteFile(std::string(511, 'A') + "evil:x:1000:1000::/h:/bin/sh\n");
    char buf[64];
    EXPECT_EQ(
        taz_passwd_name_from_uid(Path().c_str(), 1000UL, buf, sizeof(buf)), 0);
}

TEST_F(PasswdNameFromUid, ZeroBufsizeReturnsZero)
{
    WriteFile("dave:x:9:9:Dave:/home/dave:/bin/bash\n");
    char buf[64];
    EXPECT_EQ(taz_passwd_name_from_uid(Path().c_str(), 9UL, buf, 0U), 0);
}

TEST_F(PasswdNameFromUid, EmptyNameFieldIsSkipped)
{
    WriteFile(":x:9:9:NoName:/home/noname:/bin/bash\n"
              "dave:x:9:9:Dave:/home/dave:/bin/bash\n");
    char buf[64];
    EXPECT_EQ(taz_passwd_name_from_uid(Path().c_str(), 9UL, buf, sizeof(buf)),
              1);
    EXPECT_STREQ(buf, "dave");
}

TEST_F(PasswdNameFromUid, NegativeUidFieldIsSkipped)
{
    WriteFile("neg:x:-1:-1:Neg:/home/neg:/bin/bash\n");
    char buf[64];
    EXPECT_EQ(taz_passwd_name_from_uid(Path().c_str(), (unsigned long)-1, buf,
                                       sizeof(buf)),
              0);
}

TEST_F(PasswdNameFromUid, LeadingWhitespaceUidFieldIsSkipped)
{
    WriteFile("spacey:x: 7:7:Spacey:/home/spacey:/bin/bash\n");
    char buf[64];
    EXPECT_EQ(taz_passwd_name_from_uid(Path().c_str(), 7UL, buf, sizeof(buf)),
              0);
}

TEST_F(PasswdNameFromUid, LongNameIsTruncated)
{
    const std::string name = "averyverylongusernamethatdoesnotfit";
    WriteFile(name + ":x:5:5:x:/x:/bin/bash\n");
    char buf[8];
    EXPECT_EQ(taz_passwd_name_from_uid(Path().c_str(), 5UL, buf, sizeof(buf)),
              1);
    EXPECT_EQ(strlen(buf), sizeof(buf) - 1U);
    EXPECT_EQ(std::string(buf), name.substr(0, sizeof(buf) - 1U));
}

} // namespace
