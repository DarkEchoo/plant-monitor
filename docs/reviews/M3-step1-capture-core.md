# M3 Step 1 review: sensor capture core (no program yet)

| Item | Status |
|---|---|
| Author | Jesse (`src/sensors/`, branch `jesse/capture-core`, based on `origin/main` `2cb7e93`) |
| Milestone | M3 |
| Design basis | Michael's PR #5 revision `b540816`, which is **unmerged** and still marked changes-requested. Rates and heartbeats there are "proposed, to be confirmed by both partners". |
| Fresh-context agent review | Done 2026-10-26: 3 substantive findings and 5 nits, all fixed or documented (below) |
| **Michael's human review** | **PENDING. Required before merging.** Every PROVISIONAL item below needs his agreement. |
| Hardware | **None used.** Developed on Jesse's Ubuntu desktop, which has real `/dev/i2c-0..9`. No test opens them, and there is no program that could. |

## Scope

`capture_run` is the sampling loop for **one** sensor: the BH1750 or the soil sensor. The same core is meant to run later as two separate processes, one per sensor.

This slice has **no program, socket, supervisor, storage or wire format**. Devices, clocks, sleeping, the stop request and the output all come through seams. The supervisor that would restart a failed capture process **does not exist yet**.

Out of scope: analysis, calibration, light accumulation, moisture trends, fusion and plant-care advice.

| File | Purpose |
|---|---|
| `include/sensors/capture.h` | Config, seams (`capture_env`), output (`capture_sink`, `capture_sample`, `capture_heartbeat`) and outcome (`capture_outcome`) |
| `src/sensors/capture.c` | The loop, with driver adapters for both sensors |
| `tests/test_capture.c` | SIMULATED tests using the scripted mock, a fake clock, a fake stop flag and a recording sink |
| `Makefile` | `test_capture` added to `test` and `memcheck` (`asan` runs both). Nothing is added to `all`. |

## Design points

### 1. Heartbeats during backoff, and what cannot be guaranteed

- **Tick structure:**
  - Ticks fall on absolute `CLOCK_MONOTONIC` deadlines, `anchor + k × tick_period_ns`.
  - **Overrun:** if work overruns, only ticks whose deadlines have *passed* are skipped and counted, so finishing exactly on a deadline is on time. An attempt due on a skipped tick runs on the next tick, and the spacing continues from there.
  - Every tick emits its heartbeat **first**, then makes an attempt if one is due.
  - **Backoff never sleeps:** it only moves `next_attempt_tick`, and the loop still wakes and emits a heartbeat on every tick in between. A tested sensor that is down for 40 ticks still gets 40 heartbeats.
- **Not guaranteed:** a heartbeat while a call is **blocked**. The process is single-threaded, so a blocked sensor transfer, clock read or sink write delays the next heartbeat.
  - The drivers' own waits are bounded per attempt: about 513 ms for soil when the attempt includes init, and 180 ms for light.
  - A hung kernel transfer is not bounded.
  - Detecting that belongs to the future supervisor. The design proposes "hung after 30 s without a heartbeat".
- **Stop latency:** `stop_requested` is checked before the open and before every sleep, and again after an `EINTR`. A stop that arrives between the check and the start of the sleep is only seen when that sleep ends, up to one tick later. Closing that gap needs a signal-aware sleep in the future program (`ppoll`, `signalfd`); the core alone cannot.

### 2. What a sequence number counts

- **One per attempt:** each attempt takes the next `source_sequence` and produces exactly one sample. An attempt is one of:
  - an **initialization that failed** (`phase = INIT`);
  - a **measurement** (`phase = MEASURE`), with `included_init = 1` when a successful initialization ran first in the same attempt.
- **Backoff-skipped ticks** are not attempts and get no number.
- **Numbering** starts at `first_sequence`, which defaults to 1.
- **Never wraps:** when the counter would pass `UINT32_MAX`, the run stops with `SEQUENCE_EXHAUSTED`.
- **UNRESOLVED (integration):** whether numbering restarts at 1 or continues across process restarts. The caller passes `first_sequence`, so the decision can be made later without changing the core.

### 3. The startup heartbeat

- **Timing:** it is tick 0, emitted **before the device is opened**.
- **Fields:**
  - `state = STARTING`;
  - `last_sequence = 0` (no attempt yet);
  - `attempts`, `failures`, `consecutive_failures` and `missed_ticks` all 0;
  - `next_attempt_tick = 0`, `sensor_initialized = 0`;
  - `scheduled_mono_ns` = the anchor;
  - `is_simulated` from the environment alone, because the device isn't open yet.
- **After tick 0:** the first attempt follows on the same tick, and `state` becomes `OK` or `DEGRADED`.

### 4. Which failure wins when several happen

- **`capture_outcome.exit`** is the **first** fatal failure. It is never replaced.
- **The close result** is always kept separately in `close_io` and `close_attempted`. So a sink failure followed by a failed close reports `SINK_FAILED` with `close_io` holding the close errno, and neither is lost.
- **A failed close alone** turns an otherwise clean stop into `CLOSE_FAILED`.
- **Failed initial open:** returns `OPEN_FAILED` with the open errno and `cleanup_errno`. Nothing is left open. A future supervisor can then restart the process with backoff.

### 5. Rates: proposals, not requirements

- **Configuration, not constants:** `capture_config_proposed` fills in the design's **proposed** values, and the macros are named `CAPTURE_PROPOSED_*`:
  - a 10 s tick;
  - light every tick, soil every 6 ticks (60 s);
  - a backoff cap of 8 periods.

  Any caller can pass other values.
- **No cross-process alignment:** each process anchors its own schedule to its own start, so the light and soil processes aren't synchronized.
- **Soil on a 10 s tick:** soil still wakes every 10 s to send heartbeats, but attempts only every 6th tick.

### 6. Output stays independent of the storage record

`capture_sample` and `capture_heartbeat` use the drivers' own types:
- `int` stage, the stage name, the full `i2c_result` and `int` errno values;
- `int64_t` nanoseconds;
- separate `valid`, `is_simulated` and clock-trust fields.

They do **not** use the 48-byte record's u8/u16 narrowing, flag bits or layout. Mapping them onto Michael's record (or a socket message) waits until both partners agree the interface.

## Retries and reinitialization (PROVISIONAL, for Michael's review)

- **Retries:** at most one attempt per due tick, with no immediate retry.
- **Backoff:** after *n* consecutive failed attempts, the next attempt waits `min(2^(n−1), backoff_cap_periods)` sampling periods. With the proposed cap that is 1, 2, 4, 8, then 8. A valid attempt resets it.
  - The wait counts from the tick the failed attempt was **due** on, so time spent overrunning counts toward it.
  - The cap must be 1 to `CAPTURE_BACKOFF_CAP_MAX` (1024). That is a safety limit, not a proposal.
  - Proposed light cap: 80 s between attempts. Proposed soil cap: 8 minutes.
  - The core never gives up, as the design says the capture process "retries the sensor itself".
- **When init runs:**
  - before the first measurement;
  - after every failed init;
  - after a failed measurement, when an **enabled** trigger fires:

| Trigger (config) | Proposed | Why it might help | Why it might not / cost |
|---|---|---|---|
| `reinit_after_transport_failure` | on | A sensor that lost power or was reconnected needs its init again (BH1750 MTreg, soil reset). | A transport error does **not** prove the sensor lost its configuration; it may be a bus glitch. Each reinit means extra bus writes, and for soil a 500 ms software reset. |
| `reinit_after_rejections` (count) | 3 | Repeated out-of-range soil values might come from a confused sensor. | Rejections do **not** prove a reset; they may be real readings outside the accepted range. `0` disables the trigger. |

`reinit_scheduled` on a sample records that a trigger fired, so the hub and logs can see it.

## Tests: `tests/test_capture.c` (SIMULATED)

The fake world has these properties:
- **Clock:** the fake monotonic clock moves only when the mock "waits" (for example 180 ms for a light reading) or the core sleeps, and the wall clock is a fixed offset from it.
- **Ordering evidence:** each clock read records how many script steps had run.
- **Recording:** the sink records samples and heartbeats in emit order.
- **Clean-up check:** every run checks that an opened device was closed exactly once and that its mock script was consumed exactly, so there were no hidden retries and nothing ran after a failure.

| Area | Tests |
|---|---|
| Arguments | proposed config values; bad config (including a cap above 1024), missing seams and missing callbacks refused with nothing opened |
| Startup | heartbeat before the open, with the defined fields; labelled from the environment; a stop already pending at startup ends the run without opening |
| Schedule | light: 5 ticks with no drift despite 180 ms readings; soil every 6th tick with a heartbeat every tick; a 25 s overrun skips 2 ticks (counted), with no burst and with `next_attempt_tick` updated; finishing exactly on a deadline is not a miss; a 65 s soil overrun moves the attempt due on tick 6 to tick 7 |
| Clocks | wall then monotonic, only after the attempt's last step (successful and failed); sample clock failures and bad values (`tv_nsec` −1 or 10⁹, negative seconds) non-fatal; trust reported, with errno only for UNKNOWN; anchor or scheduling clock failure, a bad scheduling value, or a deadline that would overflow `int64_t` are fatal |
| Backoff | 40 ticks with init always failing: attempts on ticks 0, 1, 3, 7, 15, 23, 31, 39 and a heartbeat on all 40; a cap of 3 gives 1, 2, 3, 3; recovery resets the backoff |
| Validity | RANGE and HW_ID invalid with an OK transport and the rejected value; light failure stages and errno |
| Reinitialization | transport trigger on and off; 3 rejections on, 0 disabled; the count reset by a valid reading, after the trigger fires, and after a transport failure |
| Shutdown | EINTR without a stop re-sleeps to the same deadline; EINTR with a stop is clean; sleep failure; sink failures at startup and later; close-failure precedence both ways; sequence exhaustion |
| Labels | the 4 combinations of environment and device simulated |

## Tests-first evidence

The tests were written against the header and built with `-Werror` against a stub, which compiled cleanly. The run then failed: `test_config_proposed: Assertion 'l.tick_period_ns == INT64_C(10000000000)' failed`, exit 134.

Two problems came up in my own test code, both fixed before the core was written:
- The helper `soil_init` clashed with the driver's function name. All script helpers were renamed with a `script_` prefix.
- `test_clocks_follow_attempt` had scripted a reinit that never runs and then edited the mock afterwards. That would have bypassed the exact-consumption check, so the extra step was removed.

## Mutation checks (scratch copies in `mktemp -d` directories; repo files unchanged)

**Before the review.** The first run caught 23 of 25. Two were test gaps:
- **Backoff cap ignored:** this survived because the doubling loop stops exactly at a power-of-two cap. Fixed with `test_backoff_cap_not_power_of_two` (cap 3).
- **Trust errno always copied:** this survived because the fake seam wrote `*err` only for UNKNOWN. The fake now always writes it.

The second run caught 25 of 25.

**After the review.** The final run caught **33 of 35**. Ten new mutants came from the review: the reviewer's surviving mutants, my own cap bug, and the new guards. They are listed after the original table.

The 2 survivors are equivalent:
- **Rejection count kept after a failed init.** It is unreachable on its own: a failed init only follows a reinit, and both triggers already reset the count. The reset stays in the code to make the intent explicit.
- **An exact deadline compared with `>=` instead of `>`.** The round-up tick arithmetic gives the same tick either way. `test_overrun_exact_deadline` still pins the behaviour.

The original 25:

| Area | Mutants |
|---|---|
| Ordering | heartbeat after the attempt; open before the startup heartbeat; clocks read before the attempt |
| Schedule and backoff | backoff sleeping through ticks; cap ignored; no backoff; schedule drift; overrun burst; missed ticks not counted |
| Sequence | sequence counting skipped ticks; sequence wrapping |
| Reinitialization | transport trigger ignoring config; rejection threshold off by one; rejection count not reset; init failure leaving the sensor marked initialized |
| Results | validity from I2C status only (soil); rejected value dropped; device label ignored; trust errno always copied |
| Failure handling | close error overwriting an earlier failure; close error alone ignored; sink failure skipping close; EINTR fatal; EINTR as a stop; bad config accepted |

The 10 added after the review:

| Area | Mutants |
|---|---|
| Backoff | cap reached by stopping instead of jumping to it (my bug, below); cap limit not enforced |
| Reinitialization | rejection count kept after the trigger fires; kept after a transport failure; kept after a failed init (equivalent) |
| Clocks | `tv_nsec` range not checked; negative seconds accepted; deadline overflow not checked |
| Schedule | exact deadline counted as missed (equivalent); due attempt not moved after an overrun |
| Shutdown | no stop check before the open |

## Final verification (2026-10-26, Ubuntu desktop, GCC 15.2.0, Valgrind 3.26.0)

| Command | Exit | Result |
|---|---|---|
| `make clean`, `make` | 0 | `all` is unchanged: no capture program |
| `make test` (run twice) | 0 | 8 suites pass, including `SIMULATED capture core tests passed (scripted mock, fake clock and sink; no sensor hardware or /dev/i2c device used; proposed rates only)` |
| `make asan` | 0 | all 8 suites pass, with no ASan/UBSan reports |
| `make memcheck` | 0 | 7 binaries, each `ERROR SUMMARY: 0 errors` and `All heap blocks were freed` |
| `git diff --check` | 0 | clean |

The whole sequence was run again after the final, comment-only test edit, with the same results.

## Not verified / limitations

- **No program and no real seams.** There is no real `clock_nanosleep`, `clock_gettime`, `adjtimex` or signal handling yet, and no real-device run. Those come with the capture program, after IPC is agreed.
- **No hardware**, and **no 32-bit build** (the desktop lacks 32-bit headers). The `time_t` handling is reasoned (`intmax_t` widening), not run.
- **Blocked calls** delay heartbeats, as explained in design point 1.

## Decisions that depend on Michael (before the capture program)

1. **Socket message:** the format and framing, which side connects, and what capture does while the hub is unreachable. A sink failure is fatal for now.
2. **Heartbeats:** the message format and how the pipe is passed in (the supervisor is shared).
3. **Rates:** confirm the tick, the sampling intervals and the hung timeout.
4. **`source_sequence` across restarts:** restart at 1, or continue.
5. **Policy:** accept or change the backoff cap and both reinitialization triggers.
6. **Logs:** where text logs go. The design puts short-transfer lengths in "the text log".
7. **Mapping:** how `capture_sample` fields map onto the storage record. The design narrows errno to u16 and stores the rejected value in the sensor-value field.

## Fresh-context review (2026-10-26)

The reviewer was a separate agent with no access to this session. It was told never to open a real `/dev/i2c-N`. It ran the checks and 17 mutants of its own in a scratch copy, and changed no files. It found that all 8 required behaviours hold.

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | substantive (edge case) | `backoff_periods` doubled a `uint32_t` past 2³¹ and wrapped to 0 for very large caps, silently disabling backoff. | **Fixed.** The cap is limited to `CAPTURE_BACKOFF_CAP_MAX` (1024), and the loop jumps to the cap instead of doubling past it. My first version of the loop fix stopped at 2 for cap 3; `test_backoff_cap_not_power_of_two` caught it, and it is now a mutant. |
| 2 | substantive (test gap) | The rejection-count resets after a trigger fires and after a transport failure were untested (mutants survived). | **Fixed.** `test_rejection_count_resets`. The reset after a failed init is unreachable on its own and is recorded as an equivalent mutant. |
| 3 | substantive (test gap) | The clock-value checks (`tv_nsec` range, negative seconds) and the deadline overflow check were untested. | **Fixed.** `test_bad_clock_values` and `test_deadline_overflow` (the latter needs a 64-bit `time_t`). |
| 4 | nit (test gap) | Overrun edges: the exact-deadline boundary and the `next_attempt_tick` move after an overrun. | **Fixed.** A deadline equal to now is now on time. `test_overrun_exact_deadline`, `test_soil_overrun_moves_due_attempt` (65 s skips the due tick), and a `next_attempt_tick` assertion in `test_overrun_skips_ticks`. |
| 5 | nit | A stop arriving between the check and the sleep waits up to one tick. | **Documented** in the header and above. It needs a signal-aware sleep in the future program. |
| 6 | nit | A stop already pending at startup still opened the device and ran an attempt. | **Fixed.** A stop check before the open; `test_stop_before_open`. |
| 7 | nit | The header listed the driver waits separately, not per attempt. | **Fixed**: about 513 ms per soil attempt with init, 180 ms for light. |
| 8 | nit | Backoff counts from the due tick, so overrun time counts toward it. | **Documented** in the header and above. |

**A test fake I changed during the fixes:** the new pre-open stop check made the fake's "stop after N heartbeats" fire before the first attempt. Count-based stops in the fake now apply only once the open has been tried. A stop pending at startup is tested separately with `f.stop`.

## Michael's review

- [ ] Schedule, heartbeat and blocked-call limits (design point 1)
- [ ] Sequence semantics, and the open restart decision (design point 2)
- [ ] Backoff cap and reinitialization triggers (PROVISIONAL)
- [ ] Failure precedence and exit reasons
- [ ] Output structs kept independent of the record until the interface is agreed
- [ ] Makefile additions (shared)
- [ ] Approved for merge

Reviewer notes: _(pending)_
