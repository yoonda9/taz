/* Cross-platform helper child process for the exec tree-kill tests: spawns
 * argv[1] (the sleeper) as a plain, non-detached grandchild that inherits
 * stdio, records the grandchild's pid in the file argv[2] (newline
 * terminated, so a reader can tell it is complete), then sleeps far longer
 * than any test. Both processes hold the exec's stdout pipe, so the exec only
 * completes once the whole tree is dead. Test-only, not installed. */

#include <stdio.h>

#include <uv.h>

int main(int argc, char **argv)
{
    uv_process_t proc;
    uv_process_options_t options = {0};
    uv_stdio_container_t stdio[3];
    char *args[2];
    FILE *f;

    if (argc < 3)
    {
        return 2;
    }

    args[0] = argv[1];
    args[1] = NULL;
    stdio[0].flags = UV_INHERIT_FD;
    stdio[0].data.fd = 0;
    stdio[1].flags = UV_INHERIT_FD;
    stdio[1].data.fd = 1;
    stdio[2].flags = UV_INHERIT_FD;
    stdio[2].data.fd = 2;
    options.file = argv[1];
    options.args = args;
    options.stdio_count = 3;
    options.stdio = stdio;

    if (uv_spawn(uv_default_loop(), &proc, &options) != 0)
    {
        return 3;
    }

    f = fopen(argv[2], "w");
    if (f == NULL)
    {
        return 4;
    }
    (void)fprintf(f, "%d\n", uv_process_get_pid(&proc));
    (void)fclose(f);

    uv_sleep(60000U);
    return 0;
}
