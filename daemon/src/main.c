#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "taz/build_info.h"
#include "taz/connection.h"
#include "taz/platform.h"
#include "taz/server.h"

#define TAZ_PORT_MAX   65535
#define TAZ_STRTOL_DEC 10

static void usage(const char *prog)
{
    (void)fprintf(stderr, "Usage: %s [--host HOST] [--port PORT] [--version]\n",
                  prog);
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int port = 0;
    taz_server_t server;
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

    rc = taz_server_start(&server, host, port, taz_conn_on_new_connection);
    if (rc != 0)
    {
        (void)fprintf(stderr, "error: server start failed: %s\n",
                      uv_strerror(rc));
        return EXIT_FAILURE;
    }

    taz_platform_init_signals(&server.loop);
    uv_run(&server.loop, UV_RUN_DEFAULT);
    taz_platform_flush();
    return EXIT_SUCCESS;
}
