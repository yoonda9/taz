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

    /* Initialise server, bind to host:port, start listening.
     * Pass port=0 for an OS-assigned ephemeral port.
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
