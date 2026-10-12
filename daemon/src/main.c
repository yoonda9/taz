#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "taz/build_info.h"
#include "taz/connection.h"
#include "taz/log.h"
#include "taz/platform.h"
#include "taz/restart_args.h"
#include "taz/server.h"
#include "taz/shutdown.h"

#define TAZ_PORT_MAX      65535
#define TAZ_STRTOL_DEC    10
#define TAZ_EXEPATH_BUFSZ 4096U

static void usage(const char *prog)
{
    (void)fprintf(stderr,
                  "Usage: %s [--host HOST] [--port PORT] [--version]\n"
                  "  --host HOST  IPv4 or IPv6 literal to listen on (default "
                  "127.0.0.1; 0.0.0.0 or :: accept remote clients)\n"
                  "  --port PORT  TCP port (default 0: any free port)\n",
                  prog);
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int port = 0;
    taz_server_t server;
    char **restart_argv;
    int rc;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--version") == 0)
        {
            (void)printf("tazd %s (%s)\n", taz_build_version(), taz_build_id());
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--host") == 0)
        {
            if (i + 1 >= argc)
            {
                (void)fprintf(stderr, "error: --host requires an argument\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            host = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0)
        {
            char *endp;
            long lport;
            if (i + 1 >= argc)
            {
                (void)fprintf(stderr, "error: --port requires an argument\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            ++i;
            lport = strtol(argv[i], &endp, TAZ_STRTOL_DEC);
            if (*endp != '\0' || lport < 0 || lport > TAZ_PORT_MAX)
            {
                (void)fprintf(stderr, "error: invalid port '%s'\n", argv[i]);
                return EXIT_FAILURE;
            }
            port = (int)lport;
            continue;
        }
        (void)fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    taz_log_init();

    rc = taz_server_start(&server, host, port, taz_conn_on_new_connection);
    if (rc != 0)
    {
        taz_log(TAZ_LOG_ERROR, "server start failed: %s", uv_strerror(rc));
        taz_log_shutdown();
        return EXIT_FAILURE;
    }

    taz_shutdown_bind(&server);
    taz_platform_init_signals(&server.loop);
    taz_server_announce(&server);
    uv_run(&server.loop, UV_RUN_DEFAULT);

    rc = taz_shutdown_finish(&server);

    restart_argv = NULL;
    if (rc == 0 && taz_shutdown_mode() == TAZ_SHUTDOWN_RESTART)
    {
        char exepath[TAZ_EXEPATH_BUFSZ];
        size_t exepath_size = sizeof(exepath);

        rc = uv_exepath(exepath, &exepath_size);
        if (rc == 0)
        {
#ifdef __linux__
            taz_restart_exepath_strip_deleted(exepath);
#endif
            rc = taz_restart_args_build(argc, argv, exepath, server.port,
                                        &restart_argv);
        }
        if (rc != 0)
        {
            taz_log(TAZ_LOG_ERROR, "restart: failed to build re-exec argv");
            rc = -1;
        }
    }

    uv_library_shutdown();
    taz_platform_flush();
    taz_log_shutdown();

    if (restart_argv != NULL)
    {
        if (taz_platform_reexec(restart_argv) != 0)
        {
            (void)fprintf(stderr, "error: restart re-exec of %s failed\n",
                          restart_argv[0]);
            rc = -1;
        }
        taz_restart_args_free(restart_argv);
    }

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
