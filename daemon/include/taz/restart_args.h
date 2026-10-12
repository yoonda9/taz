#ifndef TAZ_RESTART_ARGS_H
#define TAZ_RESTART_ARGS_H

#ifdef __cplusplus
extern "C"
{
#endif

    /* Build the argv for a RESTART re-exec. argv[0] becomes exepath; every
     * other original argument (argv[1..argc-1], the command line this
     * process itself was started with) is copied through unchanged except
     * "--port", whose value is replaced with bound_port, or appended
     * ("--port", "<bound_port>") if the original command line had none -
     * so the restarted daemon always listens on the port it actually bound,
     * even when the original request was "--port 0".
     *
     * main() has already validated the original argv during this process's
     * own startup, so a dangling trailing "--port" with no value after it
     * could never reach here in practice; the builder still checks for it
     * rather than reading past the array's bound, and reports it as the one
     * failure case. On success returns 0 and sets *out_argv to a
     * NULL-terminated, heap-allocated array suitable for execv/CreateProcess
     * (argv[0] included); free it with taz_restart_args_free. On failure
     * (a dangling "--port", or an allocation failure) returns -1 and
     * *out_argv is left untouched. */
    int taz_restart_args_build(int argc, char *const argv[],
                               const char *exepath, int bound_port,
                               char ***out_argv);

    /* Free an argv array built by taz_restart_args_build. Safe to call with
     * NULL. */
    void taz_restart_args_free(char **argv);

    /* Strip a trailing " (deleted)" from exepath, in place. On Linux,
     * uv_exepath reads /proc/self/exe, which names a binary that has since
     * been replaced on disk (installed or renamed over) "<path> (deleted)";
     * the path itself then holds the new binary, which is what a restart
     * should run. main applies this on Linux only. */
    void taz_restart_exepath_strip_deleted(char *exepath);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_RESTART_ARGS_H */
