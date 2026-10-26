# M2 Step 5 review: sensor-diag hardware check

| Item | Status |
|---|---|
| Author | Jesse (`src/sensors/`, branch `jesse/sensor-diag`, based on `08daad0`, the Step 2–4 integration head of `jesse/bh1750`) |
| Milestone | M2 |
| Depends on | Draft PR #7 (`jesse/bh1750` into `main`), which is not merged yet. Until it is, a PR from this branch into `main` also shows #7's commits. |
| Fresh-context agent review | Done 2026-10-24: 2 substantive findings and 4 nits, 5 fixed and 1 accepted (below). |
| **Michael's human review** | **PENDING. Required before merging.** |
| Hardware | **None used.** Developed on Jesse's Ubuntu desktop, which has real `/dev/i2c-0` to `/dev/i2c-9`. No test opens them, and the binary was never run with a real bus. |

## Scope

`build/sensor-diag` is a one-shot command for Michael to run on the Raspberry Pi. For each sensor it opens the device once and initializes it once, then takes `--samples` measurements and prints one line per step.

It joins the Linux backend to the BH1750 and soil drivers, and does nothing else:
- no retries or reinitialization
- no capture daemon, heartbeat, socket or storage
- no analysis, calibration, trends, fusion or plant-care advice

The output is a **diagnostic format, not the storage record format**, which PR #5 still defines.

There is no `--simulate` mode in the binary. Mocks exist only in the test suite. The soil firmware VERSION query is deferred to a separate hardware diagnostic.

## Files

| File | Purpose |
|---|---|
| `include/sensors/sensor_diag.h` | config, argument parsing, the two seams (`open_device`, `read_clock`), `sensor_diag_run` |
| `src/sensors/sensor_diag.c` | the check logic and output; it doesn't use the backend or the system clocks directly |
| `src/sensors/sensor_diag_main.c` | the real seams (`i2c_linux_open`, `clock_gettime`), line-buffered stdout and the exit status. Only this file reaches `/dev/i2c-N`, and the tests never build it |
| `tests/test_sensor_diag.c` | SIMULATED tests: scripted mock plus a fake clock, run through the seams |
| `Makefile` | `build/sensor-diag` added to `all`, and `test_sensor_diag` added to `test` and `memcheck` (`asan` runs both) |

## Behaviour

- **Arguments:**
  - `--bus N` is required and has no default, because init writes to the bus (BH1750 Power On, soil reset).
  - `--samples K` is 1–100, default 1.
  - `--light-addr` is 0x23 or 0x5c. `--soil-addr` is 0x36–0x39.
  - Decimal numbers only; addresses may also be written `0x..`. A leading `0` is decimal, never octal.
  - Signs, spaces, trailing characters, repeated options and unknown arguments are errors.
  - Any argument error exits 2 before anything is opened.
- **Order:** light first, then soil. Each sensor goes open → init → K measurements → close.
  - A failed open skips that sensor; there is nothing to close.
  - A failed init skips its measurements, and the device is still closed through the `close_device` cleanup label.
  - Every measurement is attempted even after an earlier one failed.
  - One sensor failing never stops the other from being checked.
- **Validity:** `valid=1` only when the driver reports overall success (stage NONE and an OK transport result).
  - A soil HW_ID or RANGE rejection has `i2c=OK` but prints `valid=0`, with the refused value in `rejected_value`.
  - A failed reading always prints `value=none` (and `raw_count=none` for light), never a zero.
- **Failure detail:** every open, init, measure and close line prints `i2c=<status> errno=<n>`, plus `errno_text="..."` when errno is nonzero.
  - init and measure lines also print `stage=<driver stage>` and `rejected_value=<n|none>`.
  - Open lines print `cleanup_errno`.
- **Units:** light is `unit=millilux`, an integer, plus the sensor's `raw_count`. Soil is `unit=raw_counts`, uncalibrated.
- **Timestamps:**
  - **When:** `wall_ns` (`CLOCK_REALTIME`) and then `mono_ns` (`CLOCK_MONOTONIC`) are read immediately after every measurement attempt, including failed ones. They mark when the attempt *completed*.
  - **Sync:** `clock_sync=unknown` is always printed; the tool doesn't judge the wall clock.
  - **Errors:** a failed clock call prints `<clock>_ns=none <clock>_clock_errno=<n>`. A value that can't be converted (`tv_nsec` out of range, negative, or past `int64_t`) prints `<clock>_clock_value_errno=<n>`.
- **Labels:** every line starts with `SIMULATED` or `LIVE`. A line is SIMULATED if the environment *or* the device it describes is simulated. The real program always prints LIVE because it has no simulated path.
- **Exit status:** 0 if every operation succeeded.
  - 1 if any open, init, measurement, clock read or close failed, or if the output couldn't be written (checked with `fflush`/`ferror`).
  - 2 for a usage error.

Example from the tests (SIMULATED, scripted mock and fake clock; **not** a measurement), one sample of each:

```text
SIMULATED sensor=soil event=measure addr=0x36 sample=1/2 wall_ns=1005000000005 mono_ns=1006000000006 clock_sync=unknown valid=0 value=none unit=raw_counts stage=RANGE i2c=OK errno=0 rejected_value=5000
SIMULATED sensor=soil event=measure addr=0x36 sample=2/2 wall_ns=1007000000007 mono_ns=1008000000008 clock_sync=unknown valid=1 value=500 unit=raw_counts stage=NONE i2c=OK errno=0 rejected_value=none
```

## Tests: `tests/test_sensor_diag.c` (SIMULATED)

The tests use `fake_open`, which wraps `i2c_mock`, and `fake_clock`. They never call `i2c_linux_open` and never read a real clock. Each run's output goes to a `tmpfile()`; the write-failure test uses `/dev/full`.

`assert_cleaned_up` checks two things after every run:
- every opened device was closed exactly once;
- every mock script was consumed exactly, so nothing ran after a failure and nothing was skipped.

`fake_clock` records how many script steps had run at each clock read, so the tests prove the timestamps come after the measurement.

The tests cover:
- the exact full output of a 2-sample success;
- labels for all four env/device simulated combinations;
- light open failure with soil still checked;
- `cleanup_errno` reporting;
- light init failure at each of 3 steps;
- soil HW_ID rejection and RESET failure;
- soil failure at every other init and measure stage;
- soil RANGE rejection followed by a successful sample;
- light measurement failure at each step, plus a short read;
- close failure;
- clock call failure and bad clock value;
- output write failure;
- unusable config or environment;
- 6 valid and 23 invalid argument lists;
- the usage text;
- the timespec conversion limits.

The `int64_t` limit cases run only when `time_t` is 64-bit. A 32-bit `INT32_MAX` case always runs.

## Tests-first evidence

The tests were written against the header and built with `-Werror` against a stub implementation, which compiled cleanly. The first test then failed: `test_all_ok_exact_output: Assertion 'strcmp(out, expected) == 0' failed`, exit 134.

Writing the implementation exposed three test problems, all fixed without weakening any check:
- `!has(out, "value=5000")` also matched the correct `rejected_value=5000`. It became `" value=5000"`.
- The usage text wrapped two required phrases across lines. The text was rewrapped; the test's phrases are unchanged.
- One line-count formula in the new soil-stage test was wrong (it should be the total minus 6, not minus 4).

## Mutation checks (scratch copies; the repo files were not changed)

17 of 18 caught:
- Validity from I2C status only (soil)
- A failed reading printed as `value=0`
- Init failure skipping close
- Close failure not counted
- Clock failures not counted
- A value error printed as a call error
- **Clocks read before the measurement** (the reviewer's finding; it survived before the fix)
- Device label ignored
- Light failure stopping the soil check
- Output flush failure ignored
- `--bus` not required
- Sign or space accepted in numbers
- Duplicate options accepted
- Negative `tv_sec` accepted
- Rejected value dropped
- Rejected value on every soil stage
- Bad config not refused

**Survived (equivalent):** validity from I2C status only for *light*. Every BH1750 failure stage carries a non-OK transport result, because the BH1750 driver has no rejection stage, so the two rules can't differ there. Two mutants make a label or function unused and were compiled with only `-Wno-unused-label`/`-Wno-unused-function` added.

## Final verification (2026-10-24, Ubuntu desktop, GCC 15.2.0, Valgrind 3.26.0)

| Command | Exit | Result |
|---|---|---|
| `make clean` | 0 | |
| `make` | 0 | builds `build/plant-monitor` and `build/sensor-diag` |
| `make test` | 0 | 7 pass lines, including `SIMULATED sensor-diag tests passed (scripted mock and fake clock; no sensor hardware or /dev/i2c device used)` |
| `make asan` | 0 | `build/asan/`: all 7 suites pass, with no ASan/UBSan reports |
| `make memcheck` | 0 | 6 binaries, each `ERROR SUMMARY: 0 errors` and `All heap blocks were freed` |
| `make test` | 0 | 7 pass lines |
| `git diff --check` | 0 | clean |

**Manual runs of the real binary** (no real bus):
- **Usage errors:**
  - Cases: no arguments, no `--bus`, a missing value, `1x`, a repeated `--bus`, a bad soil address, `--bus=1`.
  - Each exits 2.
  - Under `strace`, the two cases containing `--bus 1` and `--help` touched no `/dev/i2c` path.
- **Absent bus:** `stat` first confirmed `/dev/i2c-4294967295` is absent. `sensor-diag --bus 4294967295 --samples 3` (normal and ASan builds):
  - Both opens fail with `errno=2`, giving `valid_readings=0/6`, and the run exits 1.
  - `strace` shows only `openat("/dev/i2c-4294967295", O_RDWR|O_CLOEXEC) = -1 ENOENT`, twice.
  - The ASan build was run without `strace`, because LeakSanitizer doesn't work under ptrace. It exits 1 with no report.
- **Output failure:** `--help > /dev/full` exits 1.

## Not verified / accepted limitations

- **No hardware.** No real transaction has happened, and the real-device path (`sensor_diag_main.c`) has no automated test.
- **No 32-bit build.** This desktop has 32-bit libc but no 32-bit development headers. The 32-bit `time_t` handling is reasoned, not run.
- **SIGPIPE:** piping into a reader that exits early (e.g. `| head`) kills the process with SIGPIPE instead of a clean exit 1. This is accepted for a one-shot tool.
- **Line buffering:** stdout is line-buffered so that a run killed during a hung transfer still keeps every line printed so far. A measurement line is printed only after its attempt returns.

## Fresh-context review (2026-10-24)

The reviewer was a separate agent with no access to this session. It was told never to open a real `/dev/i2c-N`. It ran the checks and the safe manual runs, and changed no files.

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | substantive | No test checked that the clocks are read *after* the measurement. Moving both reads before `measure()` passed the whole suite (reviewer's mutation, then reproduced). | **Fixed.** `fake_clock` records the mock's `next_step` at each read. The tests assert it after successful, failed and rejected attempts, and the mutant is now caught. |
| 2 | substantive (by reasoning) | The `int64_t` limit tests would fail on a 32-bit `time_t`, which older 32-bit Raspberry Pi OS uses. | **Fixed.** Those cases run only with `sizeof(time_t) >= 8`, and an `INT32_MAX` case always runs. The production code already widened to `intmax_t`. |
| 3 | nit | A bad clock value and a failed clock call printed the same field. | **Fixed.** They now print `<clock>_clock_value_errno` and `<clock>_clock_errno` respectively, with tests and a mutant. |
| 4 | nit | Redirected stdout is fully buffered, so a killed run could lose every line. | **Fixed.** `setvbuf(stdout, NULL, _IOLBF, 0)`, with a warning if it fails. |
| 5 | nit | No tests for the soil measure stages or the HW_ID_SELECT/WAIT/READ stages, and the RESET test didn't assert `rejected_value=none`. | **Fixed.** `test_soil_stage_failures` covers 7 stages, and the RESET assertion was added. |
| 6 | nit | Individual `fprintf` results are ignored, and SIGPIPE terminates the process. | **Accepted.** The final `fflush`/`ferror` check sets the exit status. SIGPIPE is listed under limitations. |

## Hardware check for Michael (on the Pi; nothing here has been run yet)

1. **Wiring:** with the Pi **powered off**, connect the sensors and check the connectors and pin labels (CLAUDE.md hardware contract).
2. **Find the bus:** power on and run `i2cdetect -l`. The 40-pin header's I2C is normally bus 1, but confirm it from the list. Then run `i2cdetect -y <bus>` and record whether 0x23 and 0x36 appear.
3. **Run the check:** `./build/sensor-diag --bus <bus> --samples 3`. It needs read/write access to `/dev/i2c-<bus>`. Paste the full output and the exit status exactly as printed.

**What three samples can and cannot show:** they can demonstrate basic communication and responsiveness (the addresses answer, init succeeds, and readings come back valid). They **cannot** establish timing reliability, firmware behavior, or the full valid measurement range.

## Decisions that depend on Michael's PR #5 revisions

The current diagnostic choices, which can change once the design is revised:
- **Validity:** stage NONE plus an OK transport result.
- **Failure-detail fields:** stage, transport status, errno, rejected value.
- **Timestamps:** both clocks mark attempt completion.
- **`clock_sync`:** always `unknown` until clock trust is defined.
- **Light unit:** integer millilux.

None of these is the storage format.

## Michael's review

- [ ] Output fields and labels are clear enough to run and report on the Pi
- [ ] Validity, failure detail and timestamp meaning (see the dependencies above)
- [ ] Argument rules (`--bus` required, address ranges, sample limit)
- [ ] Makefile additions (shared)
- [ ] Approved for merge

Reviewer notes: _(pending)_
