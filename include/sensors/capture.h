#ifndef SENSORS_CAPTURE_H
#define SENSORS_CAPTURE_H

#include "sensors/i2c_transport.h"

#include <stdint.h>
#include <time.h>

/*
 * Capture core: the sampling loop for ONE sensor (light or soil). The same
 * core is meant to run later as two separate processes, one per sensor.
 * There is no capture program yet, no socket, no supervisor and no
 * storage: devices, clocks, sleeping, the stop request and the output all
 * come through the seams below.
 *
 * The core captures only. Analysis, calibration, light accumulation,
 * moisture trends, fusion and plant-care advice are out of scope.
 *
 * PROVISIONAL. Every rate, backoff and reinitialization value here is a
 * proposal for review with Michael, not an agreed requirement. The design
 * (PR #5, unmerged) still lists the rates as "to be confirmed by both
 * partners".
 *
 * Schedule. Ticks fall on absolute CLOCK_MONOTONIC deadlines,
 * anchor + k * tick_period_ns, measured from the process's own start; the
 * two processes are not aligned with each other. Every tick emits a
 * heartbeat first, then makes an attempt if one is due. An attempt is due
 * every sample_every_ticks ticks (proposed: light 1, soil 6 with a 10 s
 * tick), stretched by backoff after failures. If work overruns, the ticks
 * whose deadlines have passed are counted in missed_ticks and skipped,
 * never run in a burst; finishing exactly on a deadline is on time. An
 * attempt due on a skipped tick runs on the next tick, and later attempts
 * are spaced from there.
 *
 * Heartbeats during backoff. Backoff never sleeps: it only moves the tick
 * of the next attempt, and the loop still wakes and emits a heartbeat on
 * every tick in between. What the core cannot guarantee is a heartbeat
 * while a call is blocked. The process is single-threaded, so a sensor
 * transfer, a clock read or a sink write that blocks also delays the next
 * heartbeat. The drivers' own waits are bounded per attempt (about 513 ms
 * for soil when an attempt includes init, 180 ms for light), but a hung
 * kernel transfer is not. Detecting that is the future supervisor's job
 * (it does not exist yet; the design proposes "hung after 30 s without a
 * heartbeat").
 *
 * Stop latency. stop_requested is checked before the open and before every
 * sleep, and a sleep interrupted by EINTR checks it again. A stop that
 * arrives between the check and the start of the sleep is only seen when
 * that sleep ends, up to one tick later. Closing that gap needs a
 * signal-aware sleep in the future program (for example ppoll or
 * signalfd); the core alone cannot.
 *
 * Sequence. Each attempt gets the next sequence number and produces
 * exactly one sample. An attempt is either an initialization that failed,
 * or a measurement (preceded, within the same attempt, by a successful
 * initialization when one was needed). Ticks skipped by backoff are not
 * attempts and get no number. Numbering starts at first_sequence (1 by
 * default). Whether it should restart at 1 or continue across process
 * restarts is an UNRESOLVED integration decision: the caller chooses
 * first_sequence. The core never wraps the counter; it stops with
 * CAPTURE_EXIT_SEQUENCE_EXHAUSTED instead.
 *
 * Retries and reinitialization (PROVISIONAL, configurable). There is at
 * most one attempt per due tick and no immediate retry. After n
 * consecutive failed attempts, the next attempt waits
 * min(2^(n-1), backoff_cap_periods) sampling periods (proposed: 1, 2, 4,
 * 8, then 8), counted from the tick the failed attempt was due on, so
 * time spent overrunning counts toward the wait. A successful attempt
 * resets the backoff.
 * Initialization runs before the first measurement, after a failed
 * initialization, and after a failed measurement when a trigger enabled
 * in the config fires:
 * - reinit_after_transport_failure: the measurement's transport status
 *   was not OK. A transport failure does not prove the sensor lost its
 *   configuration; reinitializing is a cautious guess, at the cost of
 *   extra bus writes and (for soil) a 500 ms software reset.
 * - reinit_after_rejections: that many consecutive measurements were
 *   rejected with an OK transport (soil RANGE). Rejections do not prove a
 *   reset either; 0 disables this trigger.
 *
 * Output. The in-memory capture_sample and capture_heartbeat use the
 * drivers' own types. They are deliberately NOT the storage-record layout
 * or a wire format, which the partners have not agreed yet.
 */

/* Proposals for review, not agreed requirements. */
#define CAPTURE_PROPOSED_TICK_PERIOD_NS   INT64_C(10000000000)  /* 10 s */
#define CAPTURE_PROPOSED_LIGHT_EVERY_TICKS 1u   /* every 10 s */
#define CAPTURE_PROPOSED_SOIL_EVERY_TICKS  6u   /* every 60 s */
#define CAPTURE_PROPOSED_BACKOFF_CAP_PERIODS 8u
#define CAPTURE_PROPOSED_REINIT_AFTER_TRANSPORT_FAILURE 1
#define CAPTURE_PROPOSED_REINIT_AFTER_REJECTIONS 3u
#define CAPTURE_DEFAULT_FIRST_SEQUENCE 1u

/*
 * Largest backoff_cap_periods accepted (a limit, not a proposal): with a
 * 10 s tick it is about 2.8 hours for light. It keeps the backoff
 * arithmetic far from overflow.
 */
#define CAPTURE_BACKOFF_CAP_MAX 1024u

typedef enum {
    CAPTURE_SENSOR_LIGHT,   /* BH1750 */
    CAPTURE_SENSOR_SOIL     /* seesaw soil sensor */
} capture_sensor;

typedef enum {
    CAPTURE_UNIT_MILLILUX,   /* integer millilux, light */
    CAPTURE_UNIT_RAW_COUNTS  /* raw capacitance counts, soil, uncalibrated */
} capture_unit;

typedef enum {
    CAPTURE_PHASE_INIT,      /* a failed initialization */
    CAPTURE_PHASE_MEASURE    /* a measurement, successful or not */
} capture_phase;

typedef enum {
    CAPTURE_CLOCK_TRUSTED,
    CAPTURE_CLOCK_UNTRUSTED,
    CAPTURE_CLOCK_TRUST_UNKNOWN   /* the check failed; see its errno */
} capture_clock_trust;

typedef enum {
    CAPTURE_STATE_STARTING,  /* no attempt has completed yet */
    CAPTURE_STATE_OK,        /* the last attempt was valid */
    CAPTURE_STATE_DEGRADED   /* the last attempt failed; in backoff */
} capture_state;

typedef enum {
    CAPTURE_CLOCK_WALL,       /* CLOCK_REALTIME */
    CAPTURE_CLOCK_MONOTONIC   /* CLOCK_MONOTONIC */
} capture_clock;

typedef enum {
    CAPTURE_EXIT_STOPPED = 0,         /* stop requested; clean */
    CAPTURE_EXIT_USAGE,               /* bad config, env or sink */
    CAPTURE_EXIT_OPEN_FAILED,         /* the initial open failed */
    CAPTURE_EXIT_CLOCK_FAILED,        /* monotonic clock for the schedule */
    CAPTURE_EXIT_SLEEP_FAILED,
    CAPTURE_EXIT_SINK_FAILED,         /* emit_sample or emit_heartbeat */
    CAPTURE_EXIT_SEQUENCE_EXHAUSTED,
    CAPTURE_EXIT_CLOSE_FAILED         /* only when nothing failed earlier */
} capture_exit;

typedef struct {
    capture_sensor sensor;
    unsigned bus_number;
    uint8_t address_7bit;            /* light 0x23/0x5c; soil 0x36-0x39 */
    int64_t tick_period_ns;          /* > 0 */
    uint32_t sample_every_ticks;     /* >= 1 */
    uint32_t backoff_cap_periods;    /* 1 to CAPTURE_BACKOFF_CAP_MAX */
    int reinit_after_transport_failure;
    uint32_t reinit_after_rejections;  /* 0: never */
    uint32_t first_sequence;         /* >= 1; see "Sequence" above */
} capture_config;

/* One attempt's result. Field meanings, not a storage or wire layout. */
typedef struct {
    capture_sensor sensor;
    uint32_t sequence;
    int is_simulated;            /* the environment or the device */
    capture_phase phase;         /* which driver call gave stage and io */
    int included_init;           /* a successful init ran first */
    int valid;                   /* the driver reported overall success */
    capture_unit unit;
    uint32_t value;              /* meaningful only when valid */
    int has_raw_count;           /* light, when valid */
    uint16_t raw_count;
    int has_rejected_value;      /* soil HW_ID or RANGE */
    uint16_t rejected_value;
    int stage;                   /* bh1750_stage or soil_stage, by sensor */
    const char *stage_name;
    i2c_result io;               /* the failing (or last) step's result */
    int64_t wall_ns;             /* read right after the attempt ... */
    int wall_errno;              /* ... nonzero: wall_ns unavailable */
    int64_t mono_ns;             /* read right after wall_ns ... */
    int mono_errno;              /* ... nonzero: mono_ns unavailable */
    capture_clock_trust clock_trust;
    int clock_trust_errno;
    int reinit_scheduled;        /* a trigger fired on this measurement, so
                                    the next attempt initializes first (a
                                    failed init always retries init) */
    uint64_t tick;               /* the tick the attempt was due on */
} capture_sample;

/*
 * One heartbeat. The startup heartbeat (tick 0) comes before the device
 * is opened: state STARTING, last_sequence 0 (no attempt yet), all counts
 * 0, sensor_initialized 0, next_attempt_tick 0, and is_simulated from the
 * environment alone.
 */
typedef struct {
    capture_sensor sensor;
    int is_simulated;
    capture_state state;
    uint64_t tick;
    int64_t scheduled_mono_ns;     /* this tick's deadline */
    uint32_t last_sequence;        /* 0: no attempt has completed */
    uint32_t consecutive_failures;
    uint64_t attempts;
    uint64_t failures;
    uint64_t missed_ticks;
    uint64_t next_attempt_tick;
    int sensor_initialized;
} capture_heartbeat;

/*
 * Where devices and time come from.
 * - open_device follows i2c_linux_open's contract.
 * - read_clock returns 0 or the errno of the failed read.
 * - sleep_until sleeps to an absolute CLOCK_MONOTONIC deadline and returns
 *   0, EINTR (the core checks stop_requested and sleeps again), or another
 *   errno (fatal).
 * - check_clock_trust returns the trust state; on UNKNOWN it sets *err.
 * - stop_requested returns nonzero once shutdown was requested.
 * - is_simulated labels output produced before the device is open.
 */
typedef struct {
    i2c_result (*open_device)(void *ctx, i2c_device *dev,
                              unsigned bus_number, uint8_t address_7bit,
                              int *cleanup_errno);
    int (*read_clock)(void *ctx, capture_clock clock, struct timespec *out);
    int (*sleep_until)(void *ctx, int64_t deadline_mono_ns);
    capture_clock_trust (*check_clock_trust)(void *ctx, int *err);
    int (*stop_requested)(void *ctx);
    void *ctx;
    int is_simulated;
} capture_env;

/*
 * Where results go; the future hub connection. Each callback returns 0 or
 * an errno. PROVISIONAL: any failure is fatal, since what to do while the
 * hub is unreachable depends on the IPC design.
 */
typedef struct {
    int (*emit_sample)(void *ctx, const capture_sample *sample);
    int (*emit_heartbeat)(void *ctx, const capture_heartbeat *heartbeat);
    void *ctx;
} capture_sink;

/*
 * How a run ended. exit is the FIRST fatal failure; a later close failure
 * never replaces it. The close result is kept separately in close_io, so
 * neither error is lost; a failed close turns an otherwise clean stop into
 * CAPTURE_EXIT_CLOSE_FAILED.
 */
typedef struct {
    capture_exit exit;
    int exit_errno;          /* errno behind exit, or 0 */
    i2c_result open_io;      /* the open result, when an open was tried */
    int open_cleanup_errno;
    int close_attempted;
    i2c_result close_io;     /* meaningful when close_attempted */
} capture_outcome;

/*
 * Fill *cfg with the PROPOSED values for sensor: a 10 s tick, light every
 * tick or soil every 6 ticks, backoff cap 8, both reinitialization
 * triggers on (rejections: 3), first_sequence 1.
 */
void capture_config_proposed(capture_config *cfg, capture_sensor sensor,
                             unsigned bus_number, uint8_t address_7bit);

/*
 * Run until a stop is requested or a fatal failure. Returns the same value
 * as out->exit. USAGE returns before anything is opened or emitted; *out
 * is always written when out is not NULL. Every device opened here is
 * closed before returning.
 */
capture_exit capture_run(const capture_config *cfg, const capture_env *env,
                         const capture_sink *sink, capture_outcome *out);

/* Stable text for logs, e.g. "DEGRADED". */
const char *capture_state_name(capture_state state);
const char *capture_exit_name(capture_exit exit);

#endif
