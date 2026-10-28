#include "capture_store.h"
#include "sensors/capture.h"
#include "sensors/capture_args.h"
#include "sensors/capture_linux.h"
#include "sensors/capture_log.h"

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

    err = capture_linux_install_stop_handlers();
    if (err != 0) {
        fprintf(stderr, "%s: installing the stop handlers failed: %s\n",
                program, strerror(err));
        return 1;
    }
    capture_linux_env(&env);

    if (capture_store_open(&store, stdout, store_dir, &cfg, &env) != 0) {
        fprintf(stderr, "%s: opening storage in '%s' failed; see the "
                        "store_error line\n", program, store_dir);
        return 1;
    }

    sink.emit_sample = capture_store_emit_sample;
    sink.emit_heartbeat = capture_store_emit_heartbeat;
    sink.ctx = &store;
    result = capture_run(&cfg, &env, &sink, &outcome);
    rc = capture_exit_status(result);

    if (capture_log_exit(stdout, cfg.sensor, env.is_simulated,
                         &outcome) != 0) {
        fprintf(stderr, "%s: writing the exit line failed\n", program);
        rc = 1;
    }
    if (capture_store_close(&store) != 0) {
        fprintf(stderr, "%s: closing storage failed; see the store_error "
                        "line\n", program);
        rc = 1;
    }
    return rc;
}
