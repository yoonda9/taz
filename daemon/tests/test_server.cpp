// Tests for server start-up: thread-pool default, listening on IPv4 and IPv6
// literals, and refusing host names.

#include <array>
#include <string>

#include <gtest/gtest.h>
#include <uv.h>

#include "taz/server.h"

namespace
{

constexpr const char *kPoolEnv = "UV_THREADPOOL_SIZE";

std::string getenv_or_empty(const char *name)
{
    std::array<char, 64> value{};
    size_t size = value.size();
    if (uv_os_getenv(name, value.data(), &size) != 0)
    {
        return {};
    }
    return {value.data(), size};
}

void on_connect(uv_stream_t * /*server*/, int /*status*/)
{
}

// Close what a successful taz_server_start opened.
void stop(taz_server_t *server)
{
    uv_close(reinterpret_cast<uv_handle_t *>(&server->handle), nullptr);
    (void)uv_run(&server->loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&server->loop), 0);
}

TEST(Server, ThreadpoolDefaultsToEightWhenUnset)
{
    ASSERT_EQ(uv_os_unsetenv(kPoolEnv), 0);
    taz_server_default_threadpool();
    EXPECT_EQ(getenv_or_empty(kPoolEnv), "8");
    (void)uv_os_unsetenv(kPoolEnv);
}

TEST(Server, ThreadpoolKeepsAnExplicitValue)
{
    ASSERT_EQ(uv_os_setenv(kPoolEnv, "3"), 0);
    taz_server_default_threadpool();
    EXPECT_EQ(getenv_or_empty(kPoolEnv), "3");
    (void)uv_os_unsetenv(kPoolEnv);
}

TEST(Server, ListensOnIpv4LiteralAndPrintsPort)
{
    taz_server_t server{};
    testing::internal::CaptureStdout();
    const int rc = taz_server_start(&server, "127.0.0.1", 0, on_connect);
    const std::string out = testing::internal::GetCapturedStdout();
    ASSERT_EQ(rc, 0) << uv_strerror(rc);
    EXPECT_EQ(out.rfind("LISTENING port=", 0), 0U) << out;
    EXPECT_NE(out, "LISTENING port=0\n");
    stop(&server);
}

TEST(Server, ListensOnIpv6Literal)
{
    taz_server_t server{};
    testing::internal::CaptureStdout();
    const int rc = taz_server_start(&server, "::1", 0, on_connect);
    const std::string out = testing::internal::GetCapturedStdout();
    if (rc == UV_EADDRNOTAVAIL || rc == UV_EAFNOSUPPORT)
    {
        GTEST_SKIP() << "no IPv6 loopback on this host: " << uv_strerror(rc);
    }
    ASSERT_EQ(rc, 0) << uv_strerror(rc);
    EXPECT_EQ(out.rfind("LISTENING port=", 0), 0U) << out;
    EXPECT_NE(out, "LISTENING port=0\n");
    stop(&server);
}

TEST(Server, RefusesAHostName)
{
    // Names would need an NSS-backed resolver; only literals are accepted.
    taz_server_t server{};
    testing::internal::CaptureStdout();
    const int rc = taz_server_start(&server, "localhost", 0, on_connect);
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, UV_EINVAL);
    EXPECT_EQ(out, "");
}

} // namespace
