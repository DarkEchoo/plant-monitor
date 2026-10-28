#define _POSIX_C_SOURCE 200809L

#include "capture_store.h"
#include "sensors/capture.h"
#include "sensors/capture_args.h"
#include "sensors/capture_linux.h"
#include "sensors/capture_log.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

/*
 * plant-capture program: one process per sensor. It runs capture_run
 * with the real Linux seams and a capture_store sink, so every result goes
 * to the text log on stdout and to this sensor's storage log. Only this
 * file reaches /dev/i2c-N, so the test suite never builds or runs it.
 *
 * TEMPORARY per-sensor storage until the hub exists; see capture_store.h.
 */

/*
 * ignore_sigpipe: a closed stdout reader then makes the log write fail
 * with EPIPE (a logged sink failure, after which storage is still flushed
 * and closed) instead of killing the process with buffered records.
 * Returns 0 or the errno of the failed sigaction.
 */
static int ignore_sigpipe(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    if (sigemptyset(&sa.sa_mask) != 0 ||
        sigaction(SIGPIPE, &sa, NULL) != 0) {
        return errno != 0 ? errno : EINVAL;
    }
    return 0;
}

/* main: parse the arguments, open storage, capture until stopped. */
int main(int argc, char **argv)
{
    const char *program =
        argc > 0 && argv[0] != NULL ? argv[0] : "plant-capture";
    capture_config cfg;
    capture_env env;
    capture_sink sink;
    capture_outcome outcome;
    capture_exit result;
    const char *store_dir = NULL;
    static capture_store store;   /* holds the writer's batch buffer */
    int err;
    int rc;

    /* Line-buffer stdout, as sensor-diag does, before any output. */
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        fprintf(stderr, "%s: warning: cannot line-buffer stdout; output "
                        "may be lost if the run is killed\n", program);
    }

    switch (capture_parse_args_store(argc, argv, &cfg, &store_dir,
                                     stderr)) {
    case CAPTURE_ARGS_HELP:
        capture_usage(stdout, program);
        return fflush(stdout) != 0 || ferror(stdout) ? 1 : 0;
    case CAPTURE_ARGS_ERROR:
        fprintf(stderr, "Try '%s --help'.\n", program);
        return 2;
    case CAPTURE_ARGS_RUN:
        break;
    }

    err = ignore_sigpipe();
    if (err != 0) {
        fprintf(stderr, "%s: ignoring SIGPIPE failed: %s\n", program,
                strerror(err));
        return 1;
    }
    err = capture_linux_install_stop_handlers();
    if (err != 0) {
        fprintf(stderr, "%s: installing the stop handlers failed: %s\n",
                program, strerror(err));
        return 1;
    }
    capture_linux_env(&env);

    err = capture_store_open(&store, stdout, store_dir, &cfg, &env);
    if (err != 0) {
        fprintf(stderr, "%s: opening storage in '%s' failed: %s\n",
                program, store_dir, strerror(err));
        return 1;
    }

    sink.emit_sample = capture_store_emit_sample;
    sink.emit_heartbeat = capture_store_emit_heartbeat;
    sink.ctx = &store;
    result = capture_run(&cfg, &env, &sink, &outcome);
    rc = capture_exit_status(result);

    if (capture_log_exit(stdout, cfg.sensor, env.is_simulated,
                         &outcome) != 0) {
        /* stdout is broken, so the exit reason goes to stderr too. */
        fprintf(stderr, "%s: writing the exit line failed; exit=%s "
                        "errno=%d\n", program, capture_exit_name(result),
                outcome.exit_errno);
        rc = 1;
    }
    err = capture_store_close(&store);
    if (err != 0 && store.closed_ok) {
        fprintf(stderr, "%s: storage was flushed and closed, but writing "
                        "its store_close line failed: %s\n", program,
                strerror(err));
        rc = 1;
    } else if (err != 0) {
        fprintf(stderr, "%s: closing storage failed: %s\n", program,
                strerror(err));
        rc = 1;
    }
    return rc;
}
