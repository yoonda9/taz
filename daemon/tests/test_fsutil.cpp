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
// taz_fsutil_truncate_utf8
// ---------------------------------------------------------------------------

TEST(TruncateUtf8, NameThatFitsIsCopiedWhole)
{
    char buf[8];
    taz_fsutil_truncate_utf8("abc", buf, sizeof(buf));
    EXPECT_STREQ(buf, "abc");
}

TEST(TruncateUtf8, AsciiOverflowTrimsAtByteBoundary)
{
    char buf[4];
    taz_fsutil_truncate_utf8("abcdef", buf, sizeof(buf));
    EXPECT_STREQ(buf, "abc");
}

TEST(TruncateUtf8, NeverSplitsAMultiByteCodepoint)
{
    // U+20AC (EUR SIGN) encodes as the 3 bytes E2 82 AC. A capacity that
    // lands inside it must drop the whole codepoint, not emit a truncated,
    // invalid tail.
    const char name[] = "ab\xE2\x82\xAC"
                        "cd";
    char buf[8];

    taz_fsutil_truncate_utf8(name, buf, 3U); // room for "ab" only
    EXPECT_STREQ(buf, "ab");

    taz_fsutil_truncate_utf8(name, buf, 4U); // 1 byte into the codepoint
    EXPECT_STREQ(buf, "ab");

    taz_fsutil_truncate_utf8(name, buf, 5U); // 2 bytes into the codepoint
    EXPECT_STREQ(buf, "ab");

    taz_fsutil_truncate_utf8(name, buf, 6U); // the full codepoint fits
    EXPECT_STREQ(buf, "ab\xE2\x82\xAC");
}

TEST(TruncateUtf8, ZeroBufsizeIsANoOp)
{
    char buf[1] = {'x'};
    taz_fsutil_truncate_utf8("abc", buf, 0U);
    EXPECT_EQ(buf[0], 'x');
}

// ---------------------------------------------------------------------------
// taz_fsutil_sanitize_utf8
// ---------------------------------------------------------------------------

namespace
{

// Sanitizes in (using in.size() as in_len) into a bufsize-byte buffer and
// returns the result as a std::string. Not for inputs whose expected
// output itself contains an embedded NUL byte: those tests must inspect
// the raw buffer instead, since std::string's own constructor would stop
// at the first NUL.
std::string SanitizeUtf8(const std::string &in, size_t bufsize)
{
    std::vector<char> buf(bufsize == 0U ? 1U : bufsize);
    taz_fsutil_sanitize_utf8(in.data(), in.size(), buf.data(), bufsize);
    return (bufsize == 0U) ? std::string() : std::string(buf.data());
}

} // namespace

TEST(SanitizeUtf8, AsciiIsCopiedVerbatim)
{
    EXPECT_EQ(SanitizeUtf8("abc", 16U), "abc");
}

TEST(SanitizeUtf8, ValidTwoByteCodepointIsCopiedVerbatim)
{
    EXPECT_EQ(SanitizeUtf8("h\xC3\xA9llo", 16U), "h\xC3\xA9llo");
}

TEST(SanitizeUtf8, ValidThreeByteCodepointIsCopiedVerbatim)
{
    EXPECT_EQ(SanitizeUtf8("\xE6\x97\xA5\xE6\x9C\xAC", 16U),
              "\xE6\x97\xA5\xE6\x9C\xAC");
}

TEST(SanitizeUtf8, ValidFourByteCodepointIsCopiedVerbatim)
{
    EXPECT_EQ(SanitizeUtf8("\xF0\x9F\x98\x80", 16U), "\xF0\x9F\x98\x80");
}

TEST(SanitizeUtf8, TwoInvalidLeadBytesBecomeTwoReplacementChars)
{
    // FF and FE are never valid lead bytes; each is its own one-byte
    // maximal subpart.
    EXPECT_EQ(SanitizeUtf8("ab\xFF"
                           "\xFE"
                           "cd",
                           32U),
              "ab\xEF\xBF\xBD\xEF\xBF"
              "\xBD"
              "cd");
}

TEST(SanitizeUtf8, StrayContinuationByteBecomesOneReplacementChar)
{
    EXPECT_EQ(SanitizeUtf8("\x80", 16U), "\xEF\xBF\xBD");
}

TEST(SanitizeUtf8, TruncatedSequenceAtEndOfInputBecomesOneReplacementChar)
{
    // E2 82 is the first two bytes of a well-formed 3-byte sequence with
    // no third byte: the maximal subpart is both bytes, replaced once.
    EXPECT_EQ(SanitizeUtf8("\xE2\x82", 16U), "\xEF\xBF\xBD");
}

TEST(SanitizeUtf8, TruncatedSequenceFollowedByAsciiIsReplacedThenCopied)
{
    EXPECT_EQ(SanitizeUtf8("\xE2\x82"
                           "A",
                           16U),
              "\xEF\xBF\xBD"
              "A");
}

TEST(SanitizeUtf8, OverlongEncodingBecomesTwoReplacementChars)
{
    // C0 AF is an overlong 2-byte encoding of '/'. C0 is never a valid
    // lead byte (maximal subpart length 1); the following AF is then a
    // stray continuation byte (also length 1).
    EXPECT_EQ(SanitizeUtf8("\xC0\xAF", 16U), "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(SanitizeUtf8, SurrogateBecomesThreeReplacementChars)
{
    // ED A0 80 would encode U+D800, a UTF-16 surrogate and never a valid
    // scalar value. ED only accepts 80-9F as its second byte, so the
    // maximal subpart is ED alone; A0 and 80 are then each a stray
    // continuation byte.
    EXPECT_EQ(SanitizeUtf8("\xED\xA0\x80", 16U),
              "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(SanitizeUtf8, OutOfRangeCodepointBecomesFourReplacementChars)
{
    // F4 90 80 80 would encode U+110000, past U+10FFFF. F4 only accepts
    // 80-8F as its second byte, so the maximal subpart is F4 alone; 90, 80
    // and 80 are then each a stray continuation byte.
    EXPECT_EQ(SanitizeUtf8("\xF4\x90\x80\x80", 16U),
              "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(SanitizeUtf8, EmbeddedNulIsCopiedAsAByte)
{
    char buf[8];
    std::memset(buf, '\x7f', sizeof(buf));
    taz_fsutil_sanitize_utf8("a\0b", 3U, buf, sizeof(buf));
    EXPECT_EQ(buf[0], 'a');
    EXPECT_EQ(buf[1], '\0');
    EXPECT_EQ(buf[2], 'b');
    EXPECT_EQ(buf[3], '\0'); // the function's own terminator
}

TEST(SanitizeUtf8, EmptyInputProducesEmptyString)
{
    EXPECT_EQ(SanitizeUtf8("", 16U), "");
}

TEST(SanitizeUtf8, ZeroBufsizeIsANoOp)
{
    char buf[1] = {'x'};
    taz_fsutil_sanitize_utf8("abc", 3U, buf, 0U);
    EXPECT_EQ(buf[0], 'x');
}

TEST(SanitizeUtf8, FourByteCodepointThatDoesNotFitIsDropped)
{
    char buf[4];
    taz_fsutil_sanitize_utf8("\xF0\x9F\x98\x80", 4U, buf, 4U);
    EXPECT_STREQ(buf, "");
}

TEST(SanitizeUtf8, FourByteCodepointThatExactlyFitsIsKept)
{
    char buf[5];
    taz_fsutil_sanitize_utf8("\xF0\x9F\x98\x80", 4U, buf, 5U);
    EXPECT_STREQ(buf, "\xF0\x9F\x98\x80");
}

TEST(SanitizeUtf8, ReplacementCharThatDoesNotFitIsDropped)
{
    // A single invalid byte needs a 3-byte U+FFFD plus the NUL (4 bytes);
    // bufsize 3 leaves room for only 2, so the replacement is dropped
    // rather than split.
    char buf[3];
    taz_fsutil_sanitize_utf8("\xFF", 1U, buf, 3U);
    EXPECT_STREQ(buf, "");
}

TEST(SanitizeUtf8, OverflowIsCutAtACodepointBoundaryNotSplitMidCodepoint)
{
    // Alternating 1-byte ASCII and 3-byte (EUR SIGN) codepoints, 4 bytes
    // per pair, built past 300 bytes. Into a 256-byte buffer (capacity
    // 255), the 64th pair's 3-byte codepoint would occupy capacity bytes
    // 253-255 - one byte past the end - so it falls right across the cut
    // and must be dropped whole, not split.
    std::string in;
    while (in.size() < 300U)
    {
        in += "a";
        in += "\xE2\x82\xAC";
    }

    std::string out = SanitizeUtf8(in, 256U);

    EXPECT_EQ(out.size(), 253U);
    EXPECT_EQ(out.back(), 'a');
    // Re-sanitizing the output must reproduce it unchanged: no partial
    // codepoint was left at the end for the validator to catch.
    EXPECT_EQ(SanitizeUtf8(out, out.size() + 1U), out);
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
// taz_fsutil_dirname
// ---------------------------------------------------------------------------

TEST(Dirname, ThreeComponents)
{
    char *out = taz_fsutil_dirname("a/b/c");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "a/b");
    free(out);
}

TEST(Dirname, TrailingSeparatorIsIgnored)
{
    char *out = taz_fsutil_dirname("a/b/");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "a");
    free(out);
}

TEST(Dirname, SingleComponentUnderRootIsRoot)
{
    char *out = taz_fsutil_dirname("/x");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "/");
    free(out);
}

TEST(Dirname, RootDirnamesToItself)
{
    char *out = taz_fsutil_dirname("/");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "/");
    free(out);
}

TEST(Dirname, NoSeparatorIsDot)
{
    char *out = taz_fsutil_dirname("x");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, ".");
    free(out);
}

TEST(Dirname, EmptyPathIsDot)
{
    char *out = taz_fsutil_dirname("");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, ".");
    free(out);
}

#ifdef _WIN32

TEST(Dirname, DriveWithBackslashRoot)
{
    char *out = taz_fsutil_dirname("C:\\x");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "C:\\");
    free(out);
}

TEST(Dirname, DriveWithForwardSlashRoot)
{
    char *out = taz_fsutil_dirname("C:/x");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "C:/");
    free(out);
}

TEST(Dirname, DriveWithTwoComponents)
{
    char *out = taz_fsutil_dirname("C:\\a\\b");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "C:\\a");
    free(out);
}

TEST(Dirname, BareDriveNoSeparatorIsDrive)
{
    char *out = taz_fsutil_dirname("C:x");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "C:");
    free(out);
}

TEST(Dirname, UncRootDirnamesToItself)
{
    char *out = taz_fsutil_dirname("\\\\server\\share\\f");
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "\\\\server\\share\\");
    free(out);
}

#endif /* _WIN32 */

// ---------------------------------------------------------------------------
// taz_fsutil_temp_name
// ---------------------------------------------------------------------------

TEST(TempName, AppendsStreamIdSuffix)
{
    char *out = taz_fsutil_temp_name("/d/f", 7U);
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "/d/f.taz-7.tmp");
    free(out);
}

TEST(TempName, MaxStreamIdIsFormattedInDecimal)
{
    char *out = taz_fsutil_temp_name("", 0xFFFFFFFFU);
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, ".taz-4294967295.tmp");
    free(out);
}

TEST(TempName, LongDestStillSucceeds)
{
    const std::string dest(1023, 'd');
    char *out = taz_fsutil_temp_name(dest.c_str(), 1U);
    ASSERT_NE(out, nullptr);
    const std::string expected = dest + ".taz-1.tmp";
    EXPECT_EQ(std::string(out), expected);
    free(out);
}

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

// ---------------------------------------------------------------------------
// taz_passwd_lookup_by_name
// ---------------------------------------------------------------------------

class PasswdLookupByName : public ::testing::Test
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

TEST_F(PasswdLookupByName, FindsMatchingNameWithUidAndGid)
{
    WriteFile("root:x:0:0:root:/root:/bin/bash\n"
              "alice:x:4242:4243:Alice:/home/alice:/bin/bash\n");
    unsigned long uid = 0;
    unsigned long gid = 0;
    EXPECT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "alice", &uid, &gid,
                                        nullptr, 0U),
              1);
    EXPECT_EQ(uid, 4242UL);
    EXPECT_EQ(gid, 4243UL);
}

TEST_F(PasswdLookupByName, NoMatchReturnsZero)
{
    WriteFile("root:x:0:0:root:/root:/bin/bash\n");
    unsigned long uid = 0;
    unsigned long gid = 0;
    EXPECT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "ghost", &uid, &gid,
                                        nullptr, 0U),
              0);
}

TEST_F(PasswdLookupByName, MissingFileReturnsZero)
{
    unsigned long uid = 0;
    unsigned long gid = 0;
    EXPECT_EQ(taz_passwd_lookup_by_name((Path() + "-missing").c_str(), "alice",
                                        &uid, &gid, nullptr, 0U),
              0);
}

TEST_F(PasswdLookupByName, MalformedAndShortLinesAreSkipped)
{
    WriteFile("no-colons-at-all\n"
              "only:one-colon\n"
              "short:x:1\n"
              "bob:x:42:42:Bob:/home/bob:/bin/bash\n");
    unsigned long uid = 0;
    unsigned long gid = 0;
    EXPECT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "bob", &uid, &gid,
                                        nullptr, 0U),
              1);
    EXPECT_EQ(uid, 42UL);
    EXPECT_EQ(gid, 42UL);
}

TEST_F(PasswdLookupByName, NamePrefixOfAnotherLineIsNotAFalseMatch)
{
    WriteFile("bobby:x:99:99:Bobby:/home/bobby:/bin/bash\n"
              "bob:x:42:42:Bob:/home/bob:/bin/bash\n");
    unsigned long uid = 0;
    unsigned long gid = 0;
    EXPECT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "bob", &uid, &gid,
                                        nullptr, 0U),
              1);
    EXPECT_EQ(uid, 42UL);
    EXPECT_EQ(gid, 42UL);
}

TEST_F(PasswdLookupByName, InjectedPathIsHonoured)
{
    WriteFile("alice:x:1000:1000:Alice:/home/alice:/bin/bash\n");

    const std::string other_path = Path() + "-other";
    std::ofstream other(other_path, std::ios::binary | std::ios::trunc);
    other << "alice:x:2000:2000:Alice:/home/alice:/bin/bash\n";
    other.close();

    unsigned long uid = 0;
    unsigned long gid = 0;
    EXPECT_EQ(taz_passwd_lookup_by_name(other_path.c_str(), "alice", &uid, &gid,
                                        nullptr, 0U),
              1);
    EXPECT_EQ(uid, 2000UL);
    EXPECT_EQ(gid, 2000UL);

    uv_fs_t req;
    (void)uv_fs_unlink(NULL, &req, other_path.c_str(), NULL);
    uv_fs_req_cleanup(&req);
}

TEST_F(PasswdLookupByName, CopiesHomeDirectory)
{
    WriteFile("alice:x:4242:4243:Alice:/home/alice:/bin/bash\n"
              "nohome:x:7:7:No Home\n"
              "noshell:x:8:8::/srv/noshell\n");
    unsigned long uid = 0;
    unsigned long gid = 0;
    char home[64];
    ASSERT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "alice", &uid, &gid,
                                        home, sizeof(home)),
              1);
    EXPECT_STREQ(home, "/home/alice");
    ASSERT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "nohome", &uid, &gid,
                                        home, sizeof(home)),
              1);
    EXPECT_STREQ(home, "");
    ASSERT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "noshell", &uid, &gid,
                                        home, sizeof(home)),
              1);
    EXPECT_STREQ(home, "/srv/noshell");
}

TEST_F(PasswdLookupByName, HomeThatDoesNotFitSkipsTheLine)
{
    WriteFile("alice:x:4242:4243:Alice:/home/alice:/bin/bash\n");
    unsigned long uid = 0;
    unsigned long gid = 0;
    char home[11]; // "/home/alice" needs 12 bytes with its NUL
    EXPECT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "alice", &uid, &gid,
                                        home, sizeof(home)),
              0);
}

TEST_F(PasswdLookupByName, IdsThatDoNotFitAValidUidAreSkipped)
{
    // 2^32 would narrow to 0 (root) and 2^32 - 1 is (uid_t)-1.
    WriteFile("wrap:x:4294967296:0:Wrap:/:/bin/sh\n"
              "minus1:x:4294967295:0:Minus1:/:/bin/sh\n"
              "gwrap:x:0:4294967296:GWrap:/:/bin/sh\n"
              "max:x:4294967294:4294967294:Max:/:/bin/sh\n");
    unsigned long uid = 0;
    unsigned long gid = 0;
    EXPECT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "wrap", &uid, &gid,
                                        nullptr, 0U),
              0);
    EXPECT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "minus1", &uid, &gid,
                                        nullptr, 0U),
              0);
    EXPECT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "gwrap", &uid, &gid,
                                        nullptr, 0U),
              0);
    ASSERT_EQ(taz_passwd_lookup_by_name(Path().c_str(), "max", &uid, &gid,
                                        nullptr, 0U),
              1);
    EXPECT_EQ(uid, TAZ_PASSWD_ID_MAX);
    EXPECT_EQ(gid, TAZ_PASSWD_ID_MAX);
}

// ---------------------------------------------------------------------------
// taz_user_name_from_uid
// ---------------------------------------------------------------------------

#ifndef _WIN32
class UserNameFromUid : public ::testing::Test
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

TEST_F(UserNameFromUid, FindsMatchingUid)
{
    WriteFile("root:x:0:0:root:/root:/bin/bash\n"
              "alice:x:1000:1000:Alice:/home/alice:/bin/bash\n");
    char buf[64];
    taz_user_name_from_uid(Path().c_str(), 1000UL, buf, sizeof(buf));
    EXPECT_STREQ(buf, "alice");
}

TEST_F(UserNameFromUid, FallsBackToDecimalWhenNotFound)
{
    WriteFile("root:x:0:0:root:/root:/bin/bash\n");
    char buf[64];
    taz_user_name_from_uid(Path().c_str(), 999UL, buf, sizeof(buf));
    EXPECT_STREQ(buf, "999");
}

TEST_F(UserNameFromUid, FallsBackToDecimalWhenFileIsMissing)
{
    char buf[64];
    taz_user_name_from_uid((Path() + "-missing").c_str(), 42UL, buf,
                           sizeof(buf));
    EXPECT_STREQ(buf, "42");
}

TEST_F(UserNameFromUid, TruncatesNameAtBufsize)
{
    WriteFile("alice:x:1000:1000:Alice:/home/alice:/bin/bash\n");
    char buf[3];
    taz_user_name_from_uid(Path().c_str(), 1000UL, buf, sizeof(buf));
    EXPECT_STREQ(buf, "al");
}

TEST_F(UserNameFromUid, ZeroBufsizeIsSilent)
{
    WriteFile("alice:x:1000:1000:Alice:/home/alice:/bin/bash\n");
    char buf[64] = "unchanged";
    taz_user_name_from_uid(Path().c_str(), 1000UL, buf, 0U);
    EXPECT_STREQ(buf, "unchanged");
}
#else
TEST(Win32AccountFromSid, CurrentProcessTokenUserIsNonEmpty)
{
    HANDLE process_token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token) == 0)
    {
        GTEST_SKIP() << "OpenProcessToken failed";
    }

    PTOKEN_USER user = nullptr;
    DWORD size = 0;
    GetTokenInformation(process_token, TokenUser, NULL, 0, &size);
    user = (PTOKEN_USER)malloc(size);
    ASSERT_NE(user, nullptr);
    ASSERT_TRUE(
        GetTokenInformation(process_token, TokenUser, user, size, &size));

    char buf[256];
    taz_win32_account_from_sid(user->User.Sid, buf, sizeof(buf));
    EXPECT_NE(strlen(buf), 0U);
    EXPECT_NE(strchr(buf, '\\'), nullptr) << "Should contain backslash";

    free(user);
    CloseHandle(process_token);
}

TEST(Win32AccountFromSid, NullSidLeavesBufferUntouched)
{
    char buf[256] = "unchanged";
    taz_win32_account_from_sid(NULL, buf, sizeof(buf));
    EXPECT_STREQ(buf, "unchanged");
}

TEST(Win32AccountFromSid, ZeroBufsizeLeavesBufferUntouched)
{
    HANDLE process_token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token) == 0)
    {
        GTEST_SKIP() << "OpenProcessToken failed";
    }

    PTOKEN_USER user = nullptr;
    DWORD size = 0;
    GetTokenInformation(process_token, TokenUser, NULL, 0, &size);
    user = (PTOKEN_USER)malloc(size);
    ASSERT_NE(user, nullptr);
    ASSERT_TRUE(
        GetTokenInformation(process_token, TokenUser, user, size, &size));

    char buf[256] = "unchanged";
    taz_win32_account_from_sid(user->User.Sid, buf, 0U);
    EXPECT_STREQ(buf, "unchanged");

    free(user);
    CloseHandle(process_token);
}
#endif

} // namespace
