// Tests for server start-up: thread-pool default, listening on IPv4 and IPv6
// literals, and refusing host names.

#include <array>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <uv.h>

#ifndef _WIN32
#include <sys/socket.h>
#endif

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
    // taz_server_start itself prints nothing; main announces the port only
    // once its signal handlers are in place.
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "");
    ASSERT_EQ(rc, 0) << uv_strerror(rc);
    EXPECT_NE(server.port, 0);

    testing::internal::CaptureStdout();
    taz_server_announce(&server);
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(out, "LISTENING port=" + std::to_string(server.port) + "\n");
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
    EXPECT_EQ(out, "");
    EXPECT_NE(server.port, 0);
    stop(&server);
}

#ifndef _WIN32
TEST(Server, ListensWithReuseaddrSet)
{
    // libuv's uv_tcp_bind sets SO_REUSEADDR on POSIX, so a RESTART
    // successor can bind while the predecessor's accepted connections sit
    // in TIME_WAIT. Windows deliberately goes without it: there it would
    // let a second, unrelated socket bind the same port while this one
    // still listens.
    taz_server_t server{};
    testing::internal::CaptureStdout();
    const int rc = taz_server_start(&server, "127.0.0.1", 0, on_connect);
    (void)testing::internal::GetCapturedStdout();
    ASSERT_EQ(rc, 0) << uv_strerror(rc);

    uv_os_fd_t fd{};
    ASSERT_EQ(uv_fileno(reinterpret_cast<uv_handle_t *>(&server.handle), &fd),
              0);
    int value = 0;
    socklen_t len = sizeof(value);
    ASSERT_EQ(getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, &len), 0);
    EXPECT_NE(value, 0);

    stop(&server);
}
#endif

TEST(Server, RetriesAnExplicitPortUntilTheFirstListenerCloses)
{
    // The bind-retry loop recreates the uv_tcp_t per attempt until the
    // predecessor's listener (still on the requested port) closes, within
    // TAZ_SERVER_BIND_RETRY_MS.
    taz_server_t first{};
    testing::internal::CaptureStdout();
    ASSERT_EQ(taz_server_start(&first, "127.0.0.1", 0, on_connect), 0);
    (void)testing::internal::GetCapturedStdout();
    const int port = first.port;

    std::thread closer(
        [&first]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            uv_close(reinterpret_cast<uv_handle_t *>(&first.handle), nullptr);
            (void)uv_run(&first.loop, UV_RUN_DEFAULT);
            (void)uv_loop_close(&first.loop);
        });

    taz_server_t second{};
    testing::internal::CaptureStdout();
    const int rc = taz_server_start(&second, "127.0.0.1", port, on_connect);
    const std::string out = testing::internal::GetCapturedStdout();
    closer.join();

    ASSERT_EQ(rc, 0) << uv_strerror(rc);
    EXPECT_EQ(second.port, port);
    EXPECT_EQ(out, "");
    stop(&second);
}

TEST(Server, FailsWithEaddrinuseWhenPortStaysHeldForTheWholeRetryWindow)
{
    // The predecessor never closes, so every retry within
    // TAZ_SERVER_BIND_RETRY_MS keeps seeing UV_EADDRINUSE and the call must
    // give up only after the whole window has elapsed, not sooner. On
    // Windows this also proves no second listener can share a live one's
    // port.
    taz_server_t first{};
    testing::internal::CaptureStdout();
    ASSERT_EQ(taz_server_start(&first, "127.0.0.1", 0, on_connect), 0);
    (void)testing::internal::GetCapturedStdout();
    const int port = first.port;

    taz_server_t second{};
    testing::internal::CaptureStdout();
    const auto started = std::chrono::steady_clock::now();
    const int rc = taz_server_start(&second, "127.0.0.1", port, on_connect);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const std::string out = testing::internal::GetCapturedStdout();

    EXPECT_EQ(rc, UV_EADDRINUSE) << uv_strerror(rc);
    EXPECT_EQ(out, "");
    EXPECT_GE(elapsed, std::chrono::milliseconds(TAZ_SERVER_BIND_RETRY_MS));

    stop(&first);
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
