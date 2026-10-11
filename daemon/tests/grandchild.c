/* POSIX-only helper child process for the exec on-exit-hook test: forks a
 * grandchild that inherits stdout and briefly outlives it, then exits right
 * away itself - so the immediate child's own exit (the on-exit hook) and
 * its stdout pipe reaching EOF (on_done) happen at two different times.
 * Test-only, not installed. */

#ifndef _WIN32
#include <unistd.h>

#include <uv.h>

int main(void)
{
    pid_t pid = fork();

    if (pid == 0)
    {
        uv_sleep(500U);
        _exit(0);
    }

    return 0;
}
#else
int main(void)
{
    return 0;
}
#endif
