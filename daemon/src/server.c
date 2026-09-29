#include "taz/server.h"

#include <stdio.h>
#include <string.h>

#define TAZ_SERVER_LISTEN_BACKLOG 128

/* Design §4.3: raised from libuv's 4 so long file and process work does not
 * starve the pool. An explicit UV_THREADPOOL_SIZE in the environment wins. */
#define TAZ_THREADPOOL_ENV     "UV_THREADPOOL_SIZE"
#define TAZ_THREADPOOL_DEFAULT "8"
#define TAZ_ENV_VALUE_MAX      32U

void taz_server_default_threadpool(void)
{
    char value[TAZ_ENV_VALUE_MAX];
    size_t size = sizeof(value);

    /* UV_ENOENT means unset; UV_ENOBUFS means set, to something long. */
    if (uv_os_getenv(TAZ_THREADPOOL_ENV, value, &size) == UV_ENOENT)
    {
        (void)uv_os_setenv(TAZ_THREADPOOL_ENV, TAZ_THREADPOOL_DEFAULT);
    }
}

/* An IPv4 or IPv6 literal. No name lookup: resolvers are NSS-backed on glibc
 * and break the static build (design §9.1). */
static int parse_host(const char *host, int port, struct sockaddr_storage *addr)
{
    (void)memset(addr, 0, sizeof(*addr));
    if (uv_ip4_addr(host, port, (struct sockaddr_in *)addr) == 0)
    {
        return 0;
    }
    return uv_ip6_addr(host, port, (struct sockaddr_in6 *)addr);
}

static int bound_port(const struct sockaddr_storage *addr)
{
    if (addr->ss_family == AF_INET6)
    {
        return (int)ntohs(((const struct sockaddr_in6 *)addr)->sin6_port);
    }
    return (int)ntohs(((const struct sockaddr_in *)addr)->sin_port);
}

int taz_server_start(taz_server_t *server, const char *host, int port,
                     uv_connection_cb on_connect)
{
    int rc;
    struct sockaddr_storage bind_addr;
    struct sockaddr_storage actual_addr;
    int addrlen;

    /* Before libuv creates its first worker. */
    taz_server_default_threadpool();

    rc = uv_loop_init(&server->loop);
    if (rc != 0)
    {
        return rc;
    }

    rc = uv_tcp_init(&server->loop, &server->handle);
    if (rc != 0)
    {
        (void)uv_loop_close(&server->loop);
        return rc;
    }
    server->handle.data = server;

    rc = parse_host(host, port, &bind_addr);
    if (rc != 0)
    {
        goto cleanup;
    }

    rc = uv_tcp_bind(&server->handle, (const struct sockaddr *)&bind_addr, 0);
    if (rc != 0)
    {
        goto cleanup;
    }

    rc = uv_listen((uv_stream_t *)&server->handle, TAZ_SERVER_LISTEN_BACKLOG,
                   on_connect);
    if (rc != 0)
    {
        goto cleanup;
    }

    addrlen = (int)sizeof(actual_addr);
    rc = uv_tcp_getsockname(&server->handle, (struct sockaddr *)&actual_addr,
                            &addrlen);
    if (rc != 0)
    {
        goto cleanup;
    }

    (void)printf("LISTENING port=%d\n", bound_port(&actual_addr));
    (void)fflush(stdout);
    return 0;

cleanup:
    uv_close((uv_handle_t *)&server->handle, NULL);
    uv_run(&server->loop, UV_RUN_DEFAULT);
    (void)uv_loop_close(&server->loop);
    return rc;
}
