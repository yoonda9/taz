#ifndef TAZ_TASK_REGISTRY_H
#define TAZ_TASK_REGISTRY_H

#include <stdint.h>

/* Daemon-wide capacity. An exited entry stays until its slot is needed, so
 * the table only refuses a new entry when every slot holds a process that
 * is still running: 64 connections each with a full table of running
 * detached tasks. */
#define TAZ_TASK_REGISTRY_CAPACITY 4096U

#ifdef __cplusplus
extern "C"
{
#endif

    /* The exit-code registry: every detached process's exit code, keyed by
     * pid plus taz_process_start_key (a reused pid has a different key), so
     * PROCESS_MONITOR on any connection can report it as authoritative.
     * Entries are independent of the task table: freeing or evicting a task
     * leaves its entry in place. Every function is thread-safe. */

    /* 1 if taz_task_registry_register would succeed right now (a free slot
     * or an exited entry to reuse), 0 if every slot holds a running
     * process. Only the loop thread registers, so on the loop thread a 1
     * here still holds for a register later in the same callback. */
    int taz_task_registry_has_room(void);

    /* Add (pid, start_key) as "running". An existing entry with the same
     * key is reused rather than duplicated. Otherwise takes a free slot, or
     * else the slot of the entry that exited longest ago. Returns 0, or
     * UV_ENOMEM when every slot holds a running process. */
    int taz_task_registry_register(uint32_t pid, uint64_t start_key);

    /* Mark the running entry matching (pid, start_key) exited with
     * exit_code (the daemon's convention: the exit status, or -signal for
     * a process killed by a signal). No matching running entry is silently
     * ignored. Called from the loop thread (an exec on-exit hook). */
    void taz_task_registry_record_exit(uint32_t pid, uint64_t start_key,
                                       int64_t exit_code);

    /* Returns 1 and sets *out_exit_code if the entry matching (pid,
     * start_key) has exited; returns 0 (leaving *out_exit_code untouched)
     * if there is no such entry or it is still running. */
    int taz_task_registry_lookup(uint32_t pid, uint64_t start_key,
                                 int64_t *out_exit_code);

    /* Returns 1 if the entry matching (pid, start_key) exists and has not
     * exited yet, 0 otherwise. Lets a caller that just saw pid disappear by
     * its own means (e.g. PROCESS_MONITOR's pidfd) tell "a registered
     * process whose exit is not recorded yet, keep waiting" apart from
     * "never registered, stop waiting". */
    int taz_task_registry_is_pending(uint32_t pid, uint64_t start_key);

    /* Test-only: drop every entry. */
    void taz_task_registry_reset_for_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_TASK_REGISTRY_H */
