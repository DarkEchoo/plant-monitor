# M3 Step 2 review: real Linux seams for the capture core

| Item | Status |
|---|---|
| Author | Jesse (`src/sensors/`, branch `jesse/capture-linux`, based on `jesse/capture-core` = PR #11 at `2cf09bd`) |
| Milestone | M3 |
| Depends on | PR #11 (capture core), which is not merged. This branch has no PR yet. |
| Fresh-context agent review | Done 2026-10-26: no substantive findings, 1 test gap and 7 nits (below) |
| **Michael's human review** | **PENDING. Required before merging.** |
| Hardware | **None used.** No test opens a real `/dev/i2c-N`. |

## Scope

These are the real seams `capture_run` needs, and nothing else (`include/sensors/capture_linux.h`, `src/sensors/capture_linux.c`, internal seam `src/sensors/capture_linux_sys.h`):

| Seam | Real call |
|---|---|
| `read_clock` | `clock_gettime(CLOCK_REALTIME / CLOCK_MONOTONIC)` |
| `sleep_until` | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`. The result is passed back, including `EINTR`. A negative deadline is `EINVAL`. A deadline a 32-bit `time_t` can't hold is `EOVERFLOW` (that path is untested on x86-64). |
| `check_clock_trust` | `docs/design.md` "Clock trust" (PR #5): `adjtimex` with `modes = 0`, read-only. Trusted only without `TIME_ERROR` and with `STA_UNSYNC` clear. A failed call is `UNKNOWN` with its errno (0 becomes `EIO`). It is never cached. |
| `open_device` | `i2c_linux_open` |
| `stop_requested` | A `sig_atomic_t` flag set by a SIGTERM/SIGINT handler that does nothing else. The handler is installed with no flags, so it stays installed for a second signal. |

There is still **no capture program**. Its output sink depends on the socket and heartbeat format, which isn't agreed yet. The stop-latency gap described in `capture.h` remains.

## Tests: `tests/test_capture_linux.c`

- **SIMULATED fake syscalls:**
  - clock mapping and errno handling;
  - the absolute sleep's clock, flags and deadline split, with `EINTR` passed back;
  - all seven clock-trust cases, and that `modes` is 0;
  - handler installation: the signal order, no flags, and failures.
- **OS PLUMBING real calls:**
  - both clocks, and a 2 ms absolute sleep;
  - a read-only `adjtimex` (the result is printed, not checked: `TRUSTED` on this desktop);
  - SIGTERM and SIGINT raised twice each, at this process only;
  - opening the absent bus `UINT_MAX` after `stat()` confirms it doesn't exist.
- **SIMULATED device, real time:** `capture_run` on the real clocks, sleep and trust check, with a scripted mock BH1750 and a 20 ms tick. Every heartbeat comes at or after its deadline, and the run stops cleanly. In the reviewer's load test (120 runs, 56 CPU hogs) it never failed.

**Test-first evidence:** built against a stub, the tests compiled under `-Werror` and failed with `test_env_filled` (exit 134).

**Mutation checks:** all 14 of my mutants are caught. Those cover trust logic, sleep flags and clock, the clock swap, the negative deadline, dropped nanoseconds, signal flags and set, ignored failures, the errno fallback, the handler not setting the flag, and the output label. The reviewer's `SA_RESETHAND` mutant is caught after the fix below. The `EOVERFLOW` mutant survives on 64-bit `time_t`, as documented.

## Fresh-context review (2026-10-26)

| # | Finding | Resolution |
|---|---|---|
| 1 | Test gap: only `SA_RESTART`/`SA_SIGINFO` were excluded; `SA_RESETHAND` survived, and with it a second SIGTERM would kill the process mid-close | **Fixed.** The fake test requires `sa_flags == 0`, and the real test raises each signal twice. The mutant is now caught. |
| 2 | The `EOVERFLOW` path is unreachable with a 64-bit `time_t` | **Documented** in the code and above. Run the tests on a 32-bit userland to cover it. |
| 3 | The comment said "no SA_RESTART" is what makes the sleep return `EINTR`; `clock_nanosleep` is never restarted anyway | **Reworded.** The comment now gives the actual reasons for using no flags. |
| 4 | No `errno = 0` before `sigemptyset` | **Fixed**, for consistency. |
| 5 | The stop-handler test left real handlers installed for later tests | **Fixed.** It now runs last. |
| 6 | The real signal → `EINTR` → stop path is not exercised end to end | **Accepted as a gap.** It is covered in parts: fake `EINTR`, the real handler flag, and the core's `EINTR` handling. |
| 7–9 | Stored-code branch: comment placement and wording | Fixed on `jesse/stable-codes` (`1868dc7`) |

## Final verification

See the commit's check run: `make clean`, `make`, `make test` (twice), `make asan` and `make memcheck`, all exit 0, with 9 suites.
