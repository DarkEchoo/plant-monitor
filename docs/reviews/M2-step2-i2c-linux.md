# M2 Step 2 review: Linux `/dev/i2c-N` transport backend

| Item | Status |
|---|---|
| Author | Jesse (`src/sensors/`, branch `jesse/i2c-linux`, based on `9392748` from `jesse/sensor-capture`) |
| Milestone | M2 (TP-D2) |
| PR #1 | Separate, still an unmerged draft. Step 2 is not published. |
| Fresh-context agent review | Two rounds, 2026-10-21 (below) |
| **Michael's human review** | **PENDING. Not reviewed or approved.** |
| Hardware | **None used.** No Raspberry Pi, sensor or I2C bus was touched. |

## What Step 2 adds

Step 2 adds no sensor drivers, storage, analysis or plant-care logic.

### Public API: `include/sensors/i2c_linux.h`

```c
i2c_result i2c_linux_open(i2c_device *dev, unsigned bus_number,
                          uint8_t address_7bit, int *cleanup_errno);
```

- **Opening a device:**
  - The device node is opened as `/dev/i2c-<bus>` with `O_RDWR | O_CLOEXEC`.
  - The address is bound with `ioctl(I2C_SLAVE)`, never `I2C_SLAVE_FORCE`. An address that a kernel driver owns therefore fails with `OS_ERROR`/`EBUSY`.
- **Checks before any syscall:**
  - An address above 0x7f is rejected.
  - So is a device that is already open, which is left untouched.
- **Ownership:** every device owns one heap context, which holds one descriptor. Drivers borrow the device. `i2c_device_close` closes the descriptor once and frees the context.
- **Failed setup:** every failure releases what was acquired, in reverse order, through cleanup labels. `dev` is never modified.
- **`cleanup_errno`:** this is required, and it is set to 0 on every return. If binding the address fails, the result keeps the bind errno, which is the cause. If the close that releases the new descriptor also fails, that close's errno goes to `*cleanup_errno`. Neither error overwrites the other, and that close is never retried.
  - This was chosen over a new `i2c_result` field. `-Wextra` would then warn about every positional `i2c_result` initializer in the transport, the mock and the tests.

### Interruption policies (also in `i2c_transport.h`)

| Operation | EINTR policy | Why |
|---|---|---|
| read / write | Reported as `OS_ERROR`/`EINTR` after one call. Not retried. | The backend cannot know whether the transaction reached the bus, so the caller decides. |
| wait_us | Retried toward the same absolute `CLOCK_MONOTONIC` deadline (`TIMER_ABSTIME`). | A wait is never cut short and never grows. Wall-clock steps (NTP, no RTC on the Pi) do not affect it. |
| close | Never retried. | Linux releases the descriptor even when `close()` fails, so a retry could close an unrelated, reused descriptor. |

A wait whose deadline would not fit `time_t` returns `INVALID_ARGUMENT`.

### Internal syscall seam: `src/sensors/i2c_linux_sys.h`

- **Contents:** eight function pointers, covering alloc, open, `ioctl_ul`, read, write, close, `clock_now` and `clock_sleep`. Each device stores its own table pointer, and there is no mutable global.
- **What it exposes:** the ioctl request, the clock id and the sleep flags pass through the seam, so tests can assert `I2C_SLAVE`, `CLOCK_MONOTONIC` and `TIMER_ABSTIME`.
- **Rule for replacements:** an injected failure must set errno, or for `clock_sleep` return the error number, exactly as the real call does. The backend copies it unchecked.
- **Scope:** the header sits under `src/`, not the public `include/`.

### Tests: `tests/test_i2c_linux.c`

| Kind | Label | What it covers |
|---|---|---|
| Fake syscalls | `SIMULATED` | The `/dev/i2c-N` path is formatted correctly for bus 0, 1 and `UINT_MAX`; open uses `I2C_SLAVE` and never `I2C_SLAVE_FORCE`; 0x7f is accepted and 0x80/0xff are rejected with zero syscalls; NULL arguments and an already-open device are rejected; allocation failure gives `ENOMEM` with no open; open failure frees the context; bind failure closes once, with `EBUSY` in the result and `EIO` in `cleanup_errno`; two devices get separate descriptors; read/write results (OK, short, 0 bytes, `EIO`, one-shot `EINTR` that is not retried); the wait deadline, including the nanosecond carry; `CLOCK_MONOTONIC` + `TIMER_ABSTIME`; EINTR retried toward the same deadline with one clock read; clock and sleep errors; the `time_t` overflow boundary; close called once on `EINTR`/`EIO`, with no syscalls afterwards |
| Real syscalls, no bus | `OS PLUMBING` | A missing path in a `mkdtemp` directory gives `ENOENT`. Real `I2C_SLAVE` on `/dev/null` gives `ENOTTY` and closes the fd. On `/dev/null` with only the ioctl faked: `FD_CLOEXEC` is set, a real write is OK, a 0-byte read is `SHORT_TRANSFER`, a real 2 ms monotonic wait passes, and close releases the fd. The public `i2c_linux_open` on bus `UINT_MAX` gives `ENOENT`, plus its pre-syscall rejections. |

**`/dev/null` checks descriptor handling only.** It is not an I2C bus, so these tests verify **no I2C transaction**.

The scripted EINTR and close failures in the fakes happen once only. A backend that wrongly retried would then see success and fail an assertion, rather than loop forever.

**Hardware safety:**
- This development machine has real `/dev/i2c-0` to `/dev/i2c-9`, which are likely display buses. No test opens any of them.
- `i2c_linux_open` always builds `/dev/i2c-<bus>`, so it cannot be pointed at `/dev/null` or a temp path. Its test uses bus `UINT_MAX`. i2c-dev minors stop at 2^20, so the kernel cannot create `/dev/i2c-4294967295`, and the test first asserts with `stat()` that the node is absent.
- strace of the suite shows the only `/dev/i2c*` open attempt is `/dev/i2c-4294967295`, which gets `ENOENT`. The only real ioctl is 0x0703 (`I2C_SLAVE`) on `/dev/null`.

### Other changed files

- **`include/sensors/i2c_transport.h`:** contract text only, covering the three interruption policies, "retrying toward the same deadline", and the overflow case. No behaviour change.
- **`Makefile` (shared):** the new `build/test_i2c_linux` is added to `test` and `memcheck`, and `asan` gets it through `test` in `build/asan/`. `all` is unchanged, so the backend is not linked into the product until a driver uses it.

## Tests-first evidence

1. With only the headers and tests written, `make test` stopped with `No rule to make target 'src/sensors/i2c_linux.c'`.
2. Linked against a throwaway stub in the scratchpad (never in the repo), the tests compiled and failed:
   `test_bus_path: Assertion 'i2c_linux_bus_path(path, sizeof path, 1) == 0' failed` (exit 134).
3. For the review fixes, the updated tests failed to compile against the old seam: incompatible hook types, and `too many arguments to function 'i2c_linux_open_path'`.

## Mutation checks (scratch copies, repo untouched, 60 s timeout each)

Each guard below was broken on a copy, and the suite failed every time.

| Broken guard | Caught by |
|---|---|
| `I2C_SLAVE_FORCE` instead of `I2C_SLAVE` | `test_open_binds_address` (request assert) |
| `CLOCK_REALTIME` everywhere | `test_wait_deadline` (clock assert) |
| relative sleep (no `TIMER_ABSTIME`) | `test_wait_deadline` (flags assert) |
| cleanup close errno not recorded | `test_bind_failure_closes_fd` |
| cleanup close errno overwrites the bind errno | `test_bind_failure_closes_fd` |
| `cleanup_errno` not cleared | `test_bind_failure_closes_fd` |
| NULL `cleanup_errno` accepted | `test_open_rejects_before_syscalls` |
| public open drops `cleanup_errno` | `test_public_open_absent_bus` |
| no close after bind failure | `test_bind_failure_closes_fd` |
| no free after open failure / on close | LeakSanitizer (`make asan`) |
| drop `O_CLOEXEC` | `test_os_dev_null_plumbing` |
| retry read / write / close on EINTR | `test_read_write_results`, `test_close_never_retried` |
| recompute the deadline on EINTR; give up the wait on EINTR | `test_wait_retries_eintr_same_deadline` |
| no address check; no already-open check | `test_open_rejects_before_syscalls` |
| drop the nanosecond carry; drop the overflow check | `test_wait_deadline`; UBSan signed-overflow report |
| drop `dev->is_simulated = 0` (round 2) | `test_open_binds_address` (stale `is_simulated = 1` on a closed device) |
| clear `cleanup_errno` only after validation, or only before bind (round 2) | `test_open_rejects_before_syscalls` (-1 sentinel) |

The first mutation run found a weak spot in the tests. EINTR fakes that failed on every call made a retrying mutant loop forever. They were changed to fail once only.

## Fresh-context review, round 1

No blocking findings.

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | substantive | Nothing tested that the code never uses `I2C_SLAVE_FORCE`. The seam hid the ioctl request, and `/dev/null` rejects both requests the same way (confirmed by mutation). | **Fixed.** The seam is now `ioctl_ul(fd, request, arg)`. Tests assert `I2C_SLAVE`, and that it is not `I2C_SLAVE_FORCE`. |
| 2 | substantive | Nothing tested `CLOCK_MONOTONIC`/`TIMER_ABSTIME`. `CLOCK_REALTIME` passed the suite, which we confirmed independently. | **Fixed.** `clock_now(clock, …)` and `clock_sleep(clock, flags, …)`, with tests asserting both. |
| 3 | nit | The cleanup `close()` result after a bind failure was discarded, against CLAUDE.md's "log every system-call error path". | **Fixed** per Jesse's refinement. The bind errno stays in the result, and the close errno goes to the required `cleanup_errno`. Close is not retried. |
| 4 | nit | `OS_ERROR` could carry errno 0 if an injected fake failed without setting errno. | **Unchanged by decision.** Real syscalls always set errno. The seam header now says injected failures must set errno like the real call. |
| 5 | nit | The contract said "remaining time" while the code uses an absolute deadline. | **Fixed.** The text now reads "retrying toward the same deadline". |
| 6 | nit | Deadline overflow returning `INVALID_ARGUMENT` was undocumented. | **Fixed** in the contract. |
| 7 | nit | The public `i2c_linux_open` was untested. | **Fixed** with `test_public_open_absent_bus`. It covers real `ENOENT` on `/dev/i2c-4294967295` behind a `stat()` guard, plus NULL dev, NULL `cleanup_errno`, 0x80 and already-open rejections. `/dev/null` cannot be reached through this entry point, because the path is always `/dev/i2c-<bus>`. |
| 8 | nit | `main` had no label comment. | **Fixed.** |

The reviewer verified by running code:
- `make test`/`asan`/`memcheck` pass.
- An `-O2 -D_FORTIFY_SOURCE=2` build passes, and `-fanalyzer` reports nothing.
- strace shows no `/dev/i2c-*` open.

## Fresh-context review, round 2

A new fresh-context reviewer, with no context from round 1, re-reviewed the fixes. It found no blocking issues, confirmed all eight round-1 findings were resolved, and confirmed that the public-entry safety argument holds:

- **i2c-dev cannot create the node:** its minors stop at 2^20, so `/dev/i2c-4294967295` cannot come from the kernel.
- **A node made by hand:** one created with `mknod`, or a udev symlink, makes `stat()` succeed, and the test aborts before `open()`.
- **Assertions stay on:** `#error` on `NDEBUG` keeps the asserts from being compiled out.

| # | Severity | Finding | Resolution |
|---|---|---|---|
| R2-1 | substantive (low) | `dev->is_simulated = 0` was untested. Every test started from `{0}`, so a reused struct from a simulated device could have labelled real readings simulated (the mutant survived). | **Fixed.** `test_open_binds_address` starts from a closed device with `is_simulated = 1` and address 0x11, then asserts both are overwritten. Mutant now killed. |
| R2-2 | substantive (low) | "`*cleanup_errno` is 0 on every return" was only partly tested, because the out-variable was already 0 (two mutants survived). | **Fixed.** A -1 sentinel is set before every rejection, allocation-failure, open-failure, success and public-entry call, and 0 is asserted afterwards. Both mutants now killed. |
| R2-3 | nit | A leak on the open-failure path is caught only by ASan/Valgrind, not plain `make test`. | Accepted. Both are mandatory in CLAUDE.md and both ran. |
| R2-4 | nit | The bus-path failure branch in `i2c_linux_open` is unreachable. | Accepted. It is commented as defensive (see Not verified). |
| R2-5 | nit | `linux_ops`, `i2c_linux_real_sys`, `fake_sys` and `plumbing_fd` lacked label comments. | **Fixed.** |
| R2-nit | nit | Clock and flags were asserted only for the first sleep, not the retried ones. | **Fixed.** Also asserted after the EINTR retries. |

The reviewer verified by running code:
- `make test`/`asan`/`memcheck` pass.
- strace shows the only `/dev/i2c*` access is a `newfstatat` and then an `open` of `/dev/i2c-4294967295`, both ENOENT. The only real ioctl is 0x0703 on `/dev/null`.
- It ran 11 more mutants, and all were killed.

It did not run a 32-bit build, because there are no `-m32` headers and no ARM cross-compiler. Its 32-bit analysis is by hand, and it agrees with round 1.

No substantive findings remain.

## Final verification

Run 2026-10-21 on Ubuntu with GCC 15.2.0, GNU Make 4.4.1 and Valgrind 3.26.0, in this order:

| Command | Exit | Result |
|---|---|---|
| `make clean` | 0 | `rm -rf build` |
| `make` | 0 | builds `build/plant-monitor` |
| `make test` | 0 | `analysis tests passed`; `SIMULATED i2c transport tests passed`; `SIMULATED linux i2c backend tests passed (fake syscalls)`; `OS PLUMBING linux i2c tests passed (/dev/null and missing paths; no I2C transaction verified, no I2C hardware used)` |
| `make asan` | 0 | `build/asan/`, all three suites pass with no ASan/UBSan/LSan reports |
| `make memcheck` | 0 | straight after `asan`, no clean: all three report `ERROR SUMMARY: 0 errors from 0 contexts` and `All heap blocks were freed` |
| `make test` | 0 | the same four pass lines |

Extra checks:
- `make clean && make -j8 asan`, then `make -j8 memcheck` with no clean: both passed, 0 errors.
- `valgrind --track-fds=yes --error-exitcode=1 ./build/test_i2c_linux` exited 0.
- No `/tmp/plant-monitor-i2c-*` directories were left behind.
- `src/analysis.c`, `include/analysis.h`, `src/main.c` and `tests/test_analysis.c` are unchanged.

## Not verified

- **No real I2C transaction, and no hardware.** No Raspberry Pi, BH1750 or seesaw sensor was connected. `i2cdetect` was not run on the target. `EBUSY` from a kernel-owned address has only been simulated.
- **No 32-bit build.** This machine has no 32-bit libc headers. The 32-bit `long`/`time_t` safety comes from reasoning only, by the author and by reviewer round 1:
  - the nanosecond sum stays below 2 × 10^9
  - `I2C_LINUX_TIME_T_MAX` is computed in `uintmax_t`
- `i2c_linux_open`'s path-format failure branch cannot trigger (`path[32]` holds any `unsigned`) and is untested.
- Fork safety: the descriptor is close-on-exec only. The header says not to `fork()` without `exec()` while a device is open.

## Michael's review

- [ ] Backend API and `cleanup_errno` approach
- [ ] Interruption policies (read/write reported, wait retried, close never retried)
- [ ] Heap context per device; syscall seam scope
- [ ] Makefile additions (shared)
- [ ] Hardware-safety approach in tests
- [ ] Approved for push and PR

Reviewer notes: _(pending)_
