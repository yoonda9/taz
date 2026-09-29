#ifndef TAZ_SERVER_H
#define TAZ_SERVER_H

#include <uv.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct taz_server_s
    {
        uv_loop_t loop;
        uv_tcp_t handle;
    } taz_server_t;

    /* Set UV_THREADPOOL_SIZE to 8 (libuv's default is 4) unless the
     * environment already sets it. Call before any thread-pool use;
     * taz_server_start does. */
    void taz_server_default_threadpool(void);

    /* Initialise server, bind to host:port, start listening. host is an IPv4
     * or IPv6 literal ("127.0.0.1", "::1", "0.0.0.0", "::"); names are not
     * resolved. Pass port=0 for an OS-assigned ephemeral port.
     * Prints "LISTENING port=<N>" to stdout and flushes on success so that
     * a test fixture can parse the actual port even when using --port 0.
     * on_connect is forwarded to uv_listen and must not be NULL.
     * Returns 0 on success, a negative libuv error code on failure. */
    int taz_server_start(taz_server_t *server, const char *host, int port,
                         uv_connection_cb on_connect);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_SERVER_H */
