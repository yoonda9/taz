// Unit tests for taz/crc32c.h: the software table path and the (when
// available) hardware-accelerated path must agree on every input, and both
// must match the published CRC32C (Castagnoli) test vectors.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "taz/crc32c.h"

namespace
{

constexpr size_t kBigBufferLen = static_cast<size_t>(1024U) * 1024U; // 1 MiB
constexpr size_t kEdgeSpan = 8U; // offsets/cuts 0..7 exercised at each end

// Fixed-seed LCG (Numerical Recipes constants): deterministic, no stdlib
// RNG state to seed/restore across test runs.
std::vector<uint8_t> MakePseudoRandomBuffer(size_t len)
{
    std::vector<uint8_t> buf(len);
    uint32_t state = 0x12345678U;

    for (size_t i = 0; i < len; i++)
    {
        state = (state * 1664525U) + 1013904223U;
        buf[i] = static_cast<uint8_t>(state >> 24U);
    }
    return buf;
}

} // namespace

TEST(Crc32c, EmptyIsZero)
{
    const char data[] = "";
    EXPECT_EQ(taz_crc32c(data, 0U), 0x00000000U);
    EXPECT_EQ(taz_crc32c_update(0U, data, 0U), 0x00000000U);
}

TEST(Crc32c, PublishedVector)
{
    const char *data = "123456789";
    EXPECT_EQ(taz_crc32c(data, std::strlen(data)), 0xE3069283U);
}

TEST(Crc32c, UpdateChainsAcrossCalls)
{
    uint32_t crc = taz_crc32c_update(0U, "1234", 4U);
    crc = taz_crc32c_update(crc, "56789", 5U);
    EXPECT_EQ(crc, 0xE3069283U);
}

TEST(Crc32c, ToLeIsLittleEndian)
{
    uint8_t out[4];
    taz_crc32c_to_le(0xE3069283U, out);
    EXPECT_EQ(out[0], 0x83U);
    EXPECT_EQ(out[1], 0x92U);
    EXPECT_EQ(out[2], 0x06U);
    EXPECT_EQ(out[3], 0xE3U);
}

TEST(Crc32c, SoftwareMatchesPublishedVector)
{
    const char *data = "123456789";
    EXPECT_EQ(taz_crc32c_software(0U, data, std::strlen(data)), 0xE3069283U);
}

TEST(Crc32c, DispatchMatchesSoftwareOverBigBuffer)
{
    std::vector<uint8_t> buf = MakePseudoRandomBuffer(kBigBufferLen);

    EXPECT_EQ(taz_crc32c(buf.data(), buf.size()),
              taz_crc32c_software(0U, buf.data(), buf.size()));
}

TEST(Crc32c, HardwareMatchesSoftwareAtUnalignedOffsetsAndLengths)
{
    if (taz_crc32c_hardware_available() == 0)
    {
        GTEST_SKIP() << "no hardware CRC32C instruction on this host";
    }

    std::vector<uint8_t> buf = MakePseudoRandomBuffer(kBigBufferLen);

    for (size_t offset = 0U; offset < kEdgeSpan; offset++)
    {
        for (size_t cut = 0U; cut < kEdgeSpan; cut++)
        {
            const size_t len = buf.size() - offset - cut;
            const uint8_t *p = buf.data() + offset;
            const uint32_t sw = taz_crc32c_software(0U, p, len);
            const uint32_t hw = taz_crc32c_hardware(0U, p, len);

            EXPECT_EQ(sw, hw) << "offset=" << offset << " cut=" << cut;
        }
    }
}
