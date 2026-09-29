#include "taz/server.h"

#include <stdio.h>

#define TAZ_SERVER_LISTEN_BACKLOG 128

int taz_server_start(taz_server_t *server, const char *host, int port,
                     uv_connection_cb on_connect)
{
    int rc;
    struct sockaddr_in bind_addr;
    struct sockaddr_in actual_addr;
    int addrlen;
    int actual_port;

    /* Set the thread-pool size before libuv creates its first worker. */
    (void)uv_os_setenv("UV_THREADPOOL_SIZE", "4");

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

    rc = uv_ip4_addr(host, port, &bind_addr);
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

    actual_port = (int)(unsigned int)ntohs(actual_addr.sin_port);
    (void)printf("LISTENING port=%d\n", actual_port);
    (void)fflush(stdout);
    return 0;

cleanup:
    uv_close((uv_handle_t *)&server->handle, NULL);
    uv_run(&server->loop, UV_RUN_DEFAULT);
    (void)uv_loop_close(&server->loop);
    return rc;
}
