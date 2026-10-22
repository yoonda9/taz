#ifndef TAZ_EXEC_H
#define TAZ_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uv.h>

#include "taz/v1/common.pb.h"

/* Sizes of taz_exec_spec_t's identity strings: a username as long as the
 * wire allows (RunAsRequest.user, CommandExecRequest.as_user), and a home
 * directory as long as any passwd line taz_passwd_lookup_by_name parses. */
#define TAZ_EXEC_USER_MAX 64U
#define TAZ_EXEC_HOME_MAX 512U

#ifdef __cplusplus
extern "C"
{
#endif

    /* Which stream a captured chunk came from. */
    typedef enum
    {
        TAZ_EXEC_STREAM_OUT = 0,
        TAZ_EXEC_STREAM_ERR = 1
    } taz_exec_stream_t;

    /* Growable pair of output buffers (OUT and ERR) sharing a single
     * combined byte cap. Zero-initialized memory is not a valid capture;
     * always call taz_exec_capture_init first. */
    typedef struct
    {
        uint8_t *out;
        size_t out_len;
        size_t out_cap;
        uint8_t *err;
        size_t err_len;
        size_t err_cap;
        size_t max_bytes;
        int truncated; /* 1 once the combined cap has been hit */
    } taz_exec_capture_t;

    /* Initialize *cap to empty OUT/ERR buffers with a combined byte cap of
     * max_bytes. */
    void taz_exec_capture_init(taz_exec_capture_t *cap, size_t max_bytes);

    /* Append len bytes from data to the OUT or ERR buffer of *cap, subject
     * to the combined max_bytes cap shared by both streams. If the append
     * would cross the cap it is applied only up to the cap, cap->truncated
     * is set, and all later appends (to either stream) become no-ops so
     * the caller can keep draining the source without growing the buffers
     * further. Allocation failure also sets truncated and drops the data
     * rather than crashing. A zero-length append is always a no-op. */
    void taz_exec_capture_append(taz_exec_capture_t *cap,
                                 taz_exec_stream_t which, const void *data,
                                 size_t len);

    /* Free the buffers owned by *cap. Safe to call on a capture that was
     * initialized but never appended to. */
    void taz_exec_capture_free(taz_exec_capture_t *cap);

    /* Opaque, heap-allocated handle for one spawned process. Owned entirely
     * by exec.c; callers only ever see a pointer to it. */
    typedef struct taz_exec_s taz_exec_t;

    /* Describes a process to spawn. The engine copies everything it needs
     * out of *spec before taz_exec_start returns, so the pointers inside it
     * (including file/args/env/cwd strings) only need to stay valid for the
     * duration of that call. */
    typedef struct
    {
        const char *file;        /* command to run */
        const char *const *args; /* arguments, WITHOUT argv[0] */
        size_t args_count;
        const taz_v1_KeyValue *env; /* extra variables, merged over the
                                     * daemon's own environment */
        size_t env_count;
        const char *cwd;     /* NULL/"" = daemon's cwd */
        uint32_t timeout_ms; /* 0 = none */
        size_t max_output_bytes;
        uv_uid_t uid; /* only used when switch_identity is set */
        uv_gid_t gid; /* only used when switch_identity is set */
        bool switch_identity;
        /* Only used when switch_identity is set, except identity_user also
         * doubles, on Windows when token is non-NULL, as the name
         * LoadUserProfileW loads the profile for (it must name the token's
         * own account - the spawn path reads it unconditionally in that
         * case, switch_identity or not). Home directory and USER/LOGNAME/
         * HOME still only come from switch_identity. */
        char identity_user[TAZ_EXEC_USER_MAX];
        char identity_home[TAZ_EXEC_HOME_MAX];
        /* Windows: a primary token (HANDLE) for a CreateProcessAsUserW
         * spawn, duplicated per call by the caller; always NULL on POSIX.
         * taz_exec_start takes ownership on entry - closed on every path
         * (failure, success, and once the child exits). On POSIX a
         * non-NULL token is simply closed and refused with UV_ENOTSUP, since
         * there is nothing there to spawn it with. */
        void *token;
    } taz_exec_spec_t;

    /* Outcome of a finished exec. out/err point into buffers owned by the
     * taz_exec_t and are valid only for the duration of the taz_exec_done_fn_t
     * call they are passed to. */
    typedef struct
    {
        int64_t exit_status;
        int term_signal;
        const uint8_t *out;
        size_t out_len;
        const uint8_t *err;
        size_t err_len;
        bool timed_out;
        bool truncated;
        bool cancelled;
    } taz_exec_result_t;

    typedef void (*taz_exec_done_fn_t)(const taz_exec_result_t *result,
                                       void *arg);

    /* Pure option-builder: when spec->switch_identity is set and this is not
     * a Windows build, sets options->uid/options->gid from spec and ORs
     * UV_PROCESS_SETUID|UV_PROCESS_SETGID into options->flags; otherwise a
     * no-op. Callers can exercise this against a bare options struct without
     * spawning anything. uv_spawn calls setgroups(0, NULL) when it switches
     * identity, so supplementary groups can never be applied through it -
     * this only ever sets the primary gid. */
    void taz_exec_apply_identity(uv_process_options_t *options,
                                 const taz_exec_spec_t *spec);

    /* Build the child's NULL-terminated NAME=VALUE environment in three
     * layers, a later one overriding an earlier one on a name match
     * (case-insensitive on Windows): a base environment; then, when
     * spec->switch_identity is set, USER and LOGNAME set to identity_user
     * and HOME to identity_home ("/" when empty), as su and sudo set them;
     * then spec->env. The base is the daemon's own environment
     * (uv_os_environ), except on Windows when spec->token is non-NULL: there
     * it is that token's own environment (CreateEnvironmentBlock(token,
     * FALSE) - never the daemon's own environment), and the USER/LOGNAME/
     * HOME trio is skipped entirely (POSIX-only; the target's loaded
     * profile sets its own equivalents). Returns 0 and stores the result in
     * *out_env (free it with taz_exec_free_env), or a UV_E* code. Spawns
     * nothing, so tests can call it directly. */
    int taz_exec_build_env(const taz_exec_spec_t *spec, char ***out_env);

    /* Free an environment from taz_exec_build_env. NULL is a no-op. */
    void taz_exec_free_env(char **env);

    /* Start *spec on loop. Returns 0 on success, in which case *out receives
     * the new handle and on_done will fire exactly once, later, from the
     * loop. Returns a UV_E* code on spawn failure, in which case on_done
     * never fires, *out is untouched, and nothing is leaked (any partially
     * initialized handles are closed internally). */
    int taz_exec_start(uv_loop_t *loop, const taz_exec_spec_t *spec,
                       taz_exec_done_fn_t on_done, void *arg, taz_exec_t **out);

    /* Kill x's whole process tree right away and mark the eventual result
     * cancelled. on_done still fires exactly once, later, once the process
     * and its pipes have finished draining - same as a natural exit or a
     * timeout. A no-op if on_done has already fired. */
    void taz_exec_cancel(taz_exec_t *x);

    /* x's child pid, matching uv_process_t.pid, or 0 once on_done has fired
     * (the process is reaped and its pid may be reused by something else). */
    uint32_t taz_exec_pid(const taz_exec_t *x);

    /* True when x was spawned through the CreateProcessAsUserW path (a
     * non-NULL token was given to taz_exec_start), false for the plain
     * uv_spawn path. Always false on POSIX. */
    bool taz_exec_as_user(const taz_exec_t *x);

    /* An on-exit hook: fn(pid, start_key, exit_status, term_signal, arg). */
    typedef void (*taz_exec_on_exit_fn_t)(uint32_t pid, uint64_t start_key,
                                          int64_t exit_status, int term_signal,
                                          void *arg);

    /* Arms an on-exit hook on x, to fire exactly once, from inside
     * on_process_exit - at process exit, strictly before the pipes finish
     * draining and before on_done (a grandchild holding stdout open delays
     * on_done, never the hook). Must be called while x's process is still
     * running (right after a successful taz_exec_start, before returning
     * control to the loop): it reads x's taz_process_start_key, so a caller
     * can key a registry on (pid, start_key) the same way the process
     * monitor does. A no-op once on_done has fired. */
    void taz_exec_set_on_exit(taz_exec_t *x, taz_exec_on_exit_fn_t fn,
                              void *arg);

    /* The start key taz_exec_set_on_exit read (the one its hook reports), or
     * 0 if it was never armed or the read failed. */
    uint64_t taz_exec_start_key(const taz_exec_t *x);

    /* Send signum to x's process group: POSIX uv_kill(-pid, signum); Windows
     * rejects an out-of-range signum with UV_EINVAL (there is no POSIX
     * signal numbering to honour, but TASK_CANCEL still needs
     * INVALID_REQUEST for an obviously-invalid value), delivers nothing for
     * signum 0 (the null signal, as on POSIX), and otherwise ignores the
     * value and terminates x's Job Object. A safe no-op once on_done has
     * fired (the pid may already be reused), returning 0. TASK_CANCEL maps
     * its own signal 0 to taz_exec_cancel instead of calling this. Returns
     * a UV_E* code. */
    int taz_exec_signal(taz_exec_t *x, int signum);

#ifdef _WIN32
    /* Test-only replacement for AssignProcessToJobObject on the
     * CreateProcessAsUserW path (job and process are HANDLEs; returns
     * nonzero on success, else 0 with the thread's last error set), to
     * drive its failure: the child must then never run. NULL restores the
     * real call. Process-global; callers must restore NULL when done. */
    typedef int (*taz_exec_assign_job_fn_t)(void *job, void *process);
    void taz_exec_set_assign_job_for_tests(taz_exec_assign_job_fn_t fn);

    /* True on a pre-Windows-8 kernel (major.minor < 6.2), where nested Job
     * Objects are unsupported. Returns the taz_exec_set_pre_win8_for_tests
     * override when one is set; otherwise asks ntdll's RtlGetVersion, which
     * reports the kernel's real version (GetVersionEx/VerifyVersionInfo are
     * manifest-shimmed and lie on Windows 8.1+ without a compat manifest).
     * Returns false if the version cannot be read. */
    bool taz_exec_win32_is_pre_win8(void);

    /* The uv_process_options_t.flags taz_exec_start spawns with:
     * UV_PROCESS_WINDOWS_HIDE, plus UV_PROCESS_DETACHED iff
     * taz_exec_win32_is_pre_win8(). */
    unsigned int taz_exec_win32_spawn_flags(void);

    /* Test-only: 1 forces "pre-Windows-8", 0 forces "Windows 8+", -1 clears
     * the override so the real version check runs again. Not synchronised;
     * call it from the loop thread before taz_exec_start. */
    void taz_exec_set_pre_win8_for_tests(int state);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TAZ_EXEC_H */
