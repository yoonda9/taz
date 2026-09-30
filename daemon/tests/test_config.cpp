// Unit tests for the daemon configuration store (config.c/h).

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "taz/config.h"
#include "taz/v1/daemon_control.pb.h"

namespace
{

class ConfigTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        taz_config_reset();
    }
};

// ---- get ----------------------------------------------------------------

TEST_F(ConfigTest, GetAllReturnsThreeDefaults)
{
    taz_v1_ConfigurationGetResponse resp;
    taz_config_get(nullptr, 0, &resp);

    ASSERT_EQ(resp.config_count, 3);

    // Collect into a map for order-independent check
    bool found_log = false;
    bool found_comp = false;
    bool found_exec = false;
    for (pb_size_t i = 0; i < resp.config_count; i++)
    {
        const taz_v1_KeyValue &kv = resp.config[i];
        if (std::string(kv.key) == "log.level")
        {
            EXPECT_STREQ(kv.value, "INFO");
            found_log = true;
        }
        else if (std::string(kv.key) == "compression")
        {
            EXPECT_STREQ(kv.value, "NONE");
            found_comp = true;
        }
        else if (std::string(kv.key) == "exec.max_output_bytes")
        {
            EXPECT_STREQ(kv.value, "1048576");
            found_exec = true;
        }
    }
    EXPECT_TRUE(found_log);
    EXPECT_TRUE(found_comp);
    EXPECT_TRUE(found_exec);
}

TEST_F(ConfigTest, GetSubsetReturnsOnlyRequestedKeys)
{
    const char *const keys[] = {"log.level"};
    taz_v1_ConfigurationGetResponse resp;
    taz_config_get(keys, 1, &resp);

    ASSERT_EQ(resp.config_count, 1);
    EXPECT_STREQ(resp.config[0].key, "log.level");
    EXPECT_STREQ(resp.config[0].value, "INFO");
}

TEST_F(ConfigTest, GetSubsetTwoKeys)
{
    const char *const keys[] = {"log.level", "exec.max_output_bytes"};
    taz_v1_ConfigurationGetResponse resp;
    taz_config_get(keys, 2, &resp);

    ASSERT_EQ(resp.config_count, 2);
}

TEST_F(ConfigTest, GetUnknownKeyIsIgnored)
{
    const char *const keys[] = {"no.such.key"};
    taz_v1_ConfigurationGetResponse resp;
    taz_config_get(keys, 1, &resp);

    EXPECT_EQ(resp.config_count, 0);
}

// ---- update + get -------------------------------------------------------

TEST_F(ConfigTest, UpdateValidThenGetReflectsChange)
{
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 1;
    std::strncpy(req.config[0].key, "log.level", sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, "DEBUG", sizeof(req.config[0].value) - 1);

    taz_v1_ConfigurationUpdateResponse uresp;
    taz_config_update(&req, &uresp);

    ASSERT_EQ(uresp.applied_count, 1);
    EXPECT_STREQ(uresp.applied[0], "log.level");
    EXPECT_EQ(uresp.rejected_count, 0);

    // Now get should reflect the new value
    const char *const keys[] = {"log.level"};
    taz_v1_ConfigurationGetResponse gresp;
    taz_config_get(keys, 1, &gresp);
    ASSERT_EQ(gresp.config_count, 1);
    EXPECT_STREQ(gresp.config[0].value, "DEBUG");
}

// ---- rejection ----------------------------------------------------------

TEST_F(ConfigTest, RejectUnknownKey)
{
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 1;
    std::strncpy(req.config[0].key, "invalid.key",
                 sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, "x", sizeof(req.config[0].value) - 1);

    taz_v1_ConfigurationUpdateResponse resp;
    taz_config_update(&req, &resp);

    EXPECT_EQ(resp.applied_count, 0);
    ASSERT_EQ(resp.rejected_count, 1);
    EXPECT_STREQ(resp.rejected[0].key, "invalid.key");
}

TEST_F(ConfigTest, RejectBadLogLevel)
{
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 1;
    std::strncpy(req.config[0].key, "log.level", sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, "VERBOSE",
                 sizeof(req.config[0].value) - 1);

    taz_v1_ConfigurationUpdateResponse resp;
    taz_config_update(&req, &resp);

    EXPECT_EQ(resp.applied_count, 0);
    ASSERT_EQ(resp.rejected_count, 1);
    EXPECT_STREQ(resp.rejected[0].key, "log.level");
    // Value should be unchanged
    const char *const keys[] = {"log.level"};
    taz_v1_ConfigurationGetResponse gresp;
    taz_config_get(keys, 1, &gresp);
    EXPECT_STREQ(gresp.config[0].value, "INFO");
}

TEST_F(ConfigTest, RejectNonNumericExecMaxOutputBytes)
{
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 1;
    std::strncpy(req.config[0].key, "exec.max_output_bytes",
                 sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, "not-a-number",
                 sizeof(req.config[0].value) - 1);

    taz_v1_ConfigurationUpdateResponse resp;
    taz_config_update(&req, &resp);

    EXPECT_EQ(resp.applied_count, 0);
    ASSERT_EQ(resp.rejected_count, 1);
    EXPECT_STREQ(resp.rejected[0].key, "exec.max_output_bytes");
}

TEST_F(ConfigTest, RejectZeroExecMaxOutputBytes)
{
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 1;
    std::strncpy(req.config[0].key, "exec.max_output_bytes",
                 sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, "0", sizeof(req.config[0].value) - 1);

    taz_v1_ConfigurationUpdateResponse resp;
    taz_config_update(&req, &resp);

    EXPECT_EQ(resp.applied_count, 0);
    ASSERT_EQ(resp.rejected_count, 1);
}

TEST_F(ConfigTest, RejectNegativeExecMaxOutputBytes)
{
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 1;
    std::strncpy(req.config[0].key, "exec.max_output_bytes",
                 sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, "-100", sizeof(req.config[0].value) - 1);

    taz_v1_ConfigurationUpdateResponse resp;
    taz_config_update(&req, &resp);

    EXPECT_EQ(resp.applied_count, 0);
    ASSERT_EQ(resp.rejected_count, 1);
}

// ---- mixed valid + invalid batch ----------------------------------------

TEST_F(ConfigTest, MixedBatchAppliesValidAndRejectsInvalid)
{
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 3;

    // Valid: update log.level
    std::strncpy(req.config[0].key, "log.level", sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, "WARN", sizeof(req.config[0].value) - 1);

    // Invalid: unknown key
    std::strncpy(req.config[1].key, "bogus.key", sizeof(req.config[1].key) - 1);
    std::strncpy(req.config[1].value, "something",
                 sizeof(req.config[1].value) - 1);

    // Valid: update exec.max_output_bytes
    std::strncpy(req.config[2].key, "exec.max_output_bytes",
                 sizeof(req.config[2].key) - 1);
    std::strncpy(req.config[2].value, "512000",
                 sizeof(req.config[2].value) - 1);

    taz_v1_ConfigurationUpdateResponse resp;
    taz_config_update(&req, &resp);

    EXPECT_EQ(resp.applied_count, 2);
    EXPECT_EQ(resp.rejected_count, 1);
    EXPECT_STREQ(resp.rejected[0].key, "bogus.key");

    // Verify applied values
    taz_v1_ConfigurationGetResponse gresp;
    taz_config_get(nullptr, 0, &gresp);
    for (pb_size_t i = 0; i < gresp.config_count; i++)
    {
        if (std::string(gresp.config[i].key) == "log.level")
        {
            EXPECT_STREQ(gresp.config[i].value, "WARN");
        }
        else if (std::string(gresp.config[i].key) == "exec.max_output_bytes")
        {
            EXPECT_STREQ(gresp.config[i].value, "512000");
        }
    }
}

// ---- reset isolates state between tests ---------------------------------

TEST_F(ConfigTest, ResetRestoresDefaults)
{
    // First update a value
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 1;
    std::strncpy(req.config[0].key, "log.level", sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, "ERROR", sizeof(req.config[0].value) - 1);
    taz_v1_ConfigurationUpdateResponse uresp;
    taz_config_update(&req, &uresp);

    // Reset and verify default is back
    taz_config_reset();
    taz_v1_ConfigurationGetResponse gresp;
    taz_config_get(nullptr, 0, &gresp);
    for (pb_size_t i = 0; i < gresp.config_count; i++)
    {
        if (std::string(gresp.config[i].key) == "log.level")
        {
            EXPECT_STREQ(gresp.config[i].value, "INFO");
        }
    }
}

// ---- valid log.level tokens ---------------------------------------------

TEST_F(ConfigTest, AllValidLogLevels)
{
    const char *levels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    for (const char *lvl : levels)
    {
        taz_config_reset();
        taz_v1_ConfigurationUpdateRequest req =
            taz_v1_ConfigurationUpdateRequest_init_zero;
        req.config_count = 1;
        std::strncpy(req.config[0].key, "log.level",
                     sizeof(req.config[0].key) - 1);
        std::strncpy(req.config[0].value, lvl, sizeof(req.config[0].value) - 1);
        taz_v1_ConfigurationUpdateResponse resp;
        taz_config_update(&req, &resp);
        EXPECT_EQ(resp.applied_count, 1) << "level=" << lvl;
        EXPECT_EQ(resp.rejected_count, 0) << "level=" << lvl;
    }
}

// ---- valid exec.max_output_bytes values ---------------------------------

TEST_F(ConfigTest, ValidExecMaxOutputBytes)
{
    taz_v1_ConfigurationUpdateRequest req =
        taz_v1_ConfigurationUpdateRequest_init_zero;
    req.config_count = 1;
    std::strncpy(req.config[0].key, "exec.max_output_bytes",
                 sizeof(req.config[0].key) - 1);
    std::strncpy(req.config[0].value, "65536", sizeof(req.config[0].value) - 1);

    taz_v1_ConfigurationUpdateResponse resp;
    taz_config_update(&req, &resp);

    EXPECT_EQ(resp.applied_count, 1);
    EXPECT_EQ(resp.rejected_count, 0);
}

} // namespace
