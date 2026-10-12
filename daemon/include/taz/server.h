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
        int port; /* bound port, set by taz_server_start on success */
    } taz_server_t;

    /* taz_server_start retries an explicit (nonzero) requested port that is
     * UV_EADDRINUSE every TAZ_SERVER_BIND_RETRY_STEP_MS, for up to
     * TAZ_SERVER_BIND_RETRY_MS total, recreating the uv_tcp_t each attempt
     * (libuv may defer a bind error into uv_listen). A RESTART successor
     * relies on this to win the race against its predecessor's still-closing
     * listener. port=0 (an OS-assigned ephemeral port) never retries. */
#define TAZ_SERVER_BIND_RETRY_STEP_MS 50U
#define TAZ_SERVER_BIND_RETRY_MS      2000U

    /* Set UV_THREADPOOL_SIZE to 8 (libuv's default is 4) unless the
     * environment already sets it. Call before any thread-pool use;
     * taz_server_start does. */
    void taz_server_default_threadpool(void);

    /* Initialise server, bind to host:port, start listening. host is an IPv4
     * or IPv6 literal ("127.0.0.1", "::1", "0.0.0.0", "::"); names are not
     * resolved. Pass port=0 for an OS-assigned ephemeral port; server->port
     * holds the bound port on success. Prints nothing: see
     * taz_server_announce.
     * on_connect is forwarded to uv_listen and must not be NULL.
     * Returns 0 on success, a negative libuv error code on failure. */
    int taz_server_start(taz_server_t *server, const char *host, int port,
                         uv_connection_cb on_connect);

    /* Print "LISTENING port=<N>" to stdout and flush, so that a test
     * fixture can parse the actual port even when using --port 0. main
     * calls it only once the signal handlers are installed: whoever reads
     * the line may stop the daemon at once, and a SIGTERM before then
     * would take the OS default action instead of the graceful walk. */
    void taz_server_announce(const taz_server_t *server);

    /* Close the listening socket (uv_is_closing guard: a no-op if already
     * closing). Part of the shutdown walk; also usable on its own. */
    void taz_server_close_listener(taz_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_SERVER_H */
