# M2 Step 1: fresh-context agent review findings

- **Reviewer:** a fresh-context Claude Code subagent (general-purpose) with
  read-only instructions. It started with no context from the authoring
  session.
- **Date:** 2026-10-19.
- **Scope:** `Makefile`, `include/sensors/i2c_transport.h`,
  `src/sensors/i2c_transport.c`, `include/sensors/i2c_mock.h`,
  `src/sensors/i2c_mock.c`, `tests/test_i2c_transport.c`.
- **Raw transcript:** kept locally by Jesse, not committed, until the course
  submission arrangement is settled.
- **Human review:** Michael requested changes on PR #1 on 2026-10-21. The
  fixes are pushed and his re-review is **PENDING**. See
  [M2-step1-i2c-transport.md](M2-step1-i2c-transport.md).

## How each finding was handled

The reviewer's line numbers did not match the files (for example, it cited
`i2c_transport.c:117-128` in a 94-line file). Each finding was checked against
the code it describes before acting on it.

| # | Severity | Finding | Resolution |
|---|---|---|---|
| S1 | substantive | "Closed even on backend error" was untested | **Fixed.** Tests assert `ops`/`ctx` are NULL and a second close returns `INVALID_ARGUMENT`. Covered for the mock (`test_unused_steps_fail_close`) and for an `OS_ERROR`/`EIO` backend (`test_failed_backend_close_still_closes`). |
| S2 | substantive | The over-transfer branch in the wrapper was unreachable through the mock | **Fixed.** A test-only canned backend shows OK with 3 of 2 bytes becomes `SHORT_TRANSFER` for read and write (`test_backend_over_transfer`). The mock rejects a script with `transfer_len > len` (`test_script_over_transfer_rejected`). The header comment now names over-transfer. |
| S3 | substantive | `OS_ERROR` could come back with `os_errno == 0` | **Fixed by contract.** The header requires `OS_ERROR` to carry the nonzero errno of the failed call. A condition with no errno, such as `read()` returning 0, is reported as OK with the real byte count, which becomes `SHORT_TRANSFER`. The mock produces `OS_ERROR` only from a nonzero `fail_errno`. The wrapper does not invent an errno. |
| S4 | substantive | Byte counts in wait results were undefined | **Fixed.** The wrapper forces both counts to 0. The header requires backends to retry `EINTR` with the remaining time and never report OK for a partial wait. `test_wait_has_no_byte_counts` was confirmed failing (exit 134) without the fix. |
| S5 | substantive | No test for a wrong read length or an over-minimum wait | **Fixed.** Added `test_read_length_mismatch` and `test_long_wait_recorded`. |
| N1 | nit | `i2c_mock_open` results were ignored | **Fixed.** Every open result is asserted. |
| N2 | nit | Some close results were ignored | **Fixed.** The expected close status is asserted. |
| N3 | nit | The short-read test did not check the copied byte | **Fixed.** It now asserts `buf[0] == 0x02`. |
| N4 | nit | The mock's validation order differed between read and write | **Documented** in `i2c_mock.h`. |
| N5 | nit, upgraded | `i2c_mock_open` overwrote a device that was already open | **Fixed** (see below). |
| N6 | nit | The Makefile does not rebuild when CFLAGS change | **Fixed after Michael's PR #1 review.** He asked for the separate `build/asan` directory. `asan` now builds there through a recursive make, so `memcheck` and `test` keep using normal `build/` binaries, and `asan` no longer depends on `clean`. See [M2-step1-i2c-transport.md](M2-step1-i2c-transport.md#n6-stale-sanitizer-binaries-resolved-after-michaels-review). |

### N5: why it was upgraded from a nit

Reopening a device that was already open overwrote `dev->ops`/`dev->ctx`
without complaint. With the mock, the old binding was dropped and every test
still passed. With the planned Linux backend, the same caller bug would
overwrite the only copy of an open `/dev/i2c-1` descriptor. That descriptor
would be orphaned, which CLAUDE.md forbids. The mock was therefore hiding an
ownership bug that real hardware would turn into a leak.

The fix keeps the mock and the planned Linux backend on the same contract,
which is stated in `i2c_transport.h`:

- A device starts closed: zero-initialize it with `i2c_device dev = {0};`.
- Every backend's open refuses a device whose `ops` is non-NULL. It returns
  `INVALID_ARGUMENT` and leaves the device untouched.
- A device closed with `i2c_device_close` can be opened again.

`test_open_refuses_open_device` covers this. It was written first and failed
against the old mock (`Assertion ... == I2C_STATUS_INVALID_ARGUMENT failed`).
The test checks that the first binding is intact: the context, the address
and the unconsumed script are unchanged, and the device still works. It also
checks that reopening after a close succeeds.

The mock does not detect one `i2c_mock` being bound to two devices, because a
caller-owned mock has no reliable "unused" state. This is a test-only concern:
each Linux device will own its own descriptor.

## Reviewer report (verbatim)

> No correctness bugs that block Step 1. The open findings are contract gaps and missing tests. I checked each claim by compiling a backend that returns bad results on purpose (`exp.c` in the scratch review directory) and by running make in a copy of the repo there. The repo itself is untouched.
>
> **Verified results:**
> - `make test` passes in the copy. Output ends with `SIMULATED i2c transport tests passed`.
> - `make asan` passes. The new test is rebuilt with `-fsanitize=address,undefined -g`, and so is the rest of the build. It also passes with `-j8`.
> - `make memcheck` passes from a clean build: 0 errors for both binaries.
>
> ### Substantive findings
>
> **S1. "Marked closed even on backend error" has no test** (`src/sensors/i2c_transport.c:117-128`)
> - The code works. With a backend whose close returns OS_ERROR/errno 5, I saw `ops == NULL` afterwards. A failed mock close also gave `ops == NULL`.
> - No test asserts `dev.ops == NULL` after a failed close (`test_unused_steps_fail_close`, `test_write_bytes_mismatch`).
> - Risk: a later refactor to "only clear on success" would pass the suite, and once the Linux backend exists that means a second `close()` on a reused fd.
> - Fix: add `assert(dev.ops == NULL);` and a check that a second close returns INVALID_ARGUMENT, after the failing close in `test_unused_steps_fail_close`.
>
> **S2. The wrapper's over-transfer branch is untested and the mock can never reach it** (`i2c_transport.c:72-73`; mock lines 83 and 100)
> - A backend returning OK with transferred 3 for requested 2 does come back as SHORT_TRANSFER (verified).
> - The mock treats `transfer_len > len` as SCRIPT_MISMATCH, so no test reaches that branch. The SCRIPT_MISMATCH path for a script with `transfer_len > len` is also untested.
> - Fix: add a small test-only ops struct in the test file whose read returns `transferred_len = len + 1`, and assert SHORT_TRANSFER.
> - Nit: the name SHORT_TRANSFER is wrong for an over-transfer. Either reword the header comment to "transfer length mismatch" or document it. By the time this fires the buffer has already been overrun, so it is a diagnostic, not protection.
>
> **S3. OS_ERROR can come back with `os_errno == 0`** (`i2c_transport.c:69-77`)
> - A backend that returns `{OS_ERROR, 0, ...}` is passed through unchanged (verified). The header promises "see os_errno", but the errno can be lost and nothing checks it.
> - The Linux backend will meet this: a `read()` returning 0 is not an error with an errno, and a logging call between the syscall and capturing errno would overwrite it.
> - Fix: in `finish_transfer` and `wait_us`, if `status == OS_ERROR && os_errno == 0`, set `os_errno = EIO`. Or state in the header that backends must never return OS_ERROR with errno 0, and enforce it in the mock with a test.
>
> **S4. What `wait_us` puts in `requested_len`/`transferred_len` is undefined** (`i2c_transport.c:104-110`, `i2c_transport.h:36`)
> - The wrapper passes backend values straight through. A backend returning 99/77 came out as 99/77 (verified). The mock returns 0/0.
> - This needs deciding before the Linux backend, because nanosleep EINTR handling will want somewhere to report "slept X of Y µs".
> - Fix: either have the wrapper force both fields to 0 for waits, or define them as µs requested/slept. Document it in the header and assert it in `test_full_script_succeeds`. Also say that the backend must retry on EINTR with the remaining time and never report OK for a partial sleep.
>
> **S5. Missing coverage for other behavior the mock claims** (test file)
> - No test for a read whose length differs from the script (mock line 100, `step->len != len`).
> - No test that an over-long wait is accepted and the actual duration is recorded (for example, wait 5000 against min 3000, expect `waited_us == 5000`).
> - Fix: one small test for each.
>
> ### Nits
> - **N1.** Tests at lines 68, 88, 109, 129, 150, 168, 185, 206, 226 and 238 ignore the return value of `i2c_mock_open`. If open failed, `dev` would be uninitialized and the next call would be undefined behavior. Wrap each in `assert(... == I2C_STATUS_OK)`.
> - **N2.** Lines 172, 189 and 212 ignore the close result. Assert the expected SCRIPT_MISMATCH or OK so the sticky state is visible at close.
> - **N3.** `test_short_read` checks `buf[1]` but not `buf[0] == 0x02`, so it never confirms the partial byte was copied.
> - **N4.** Mock check order is inconsistent between paths:
>   - A WRITE step with `bytes == NULL` and `fail_errno` set gives SCRIPT_MISMATCH, not the scripted OS_ERROR (verified).
>   - A READ step allows NULL bytes when `transfer_len == 0`.
>   - This is defensible, because a write needs expected bytes to compare, but it belongs in a comment in `i2c_mock.h`.
> - **N5.** `i2c_mock_open` overwrites `dev` without checking whether it is already open. That is harmless for the mock, but the Linux open should refuse an open device or fds will leak.
> - **N6.** Makefile, an existing pattern that now also covers the new target:
>   - Nothing rebuilds when CFLAGS change, so after `make asan` the `build/` binaries are ASan builds.
>   - `make memcheck` then runs valgrind on those binaries and fails with `Error 1`. I reproduced this.
>   - `make test` after asan also silently reuses the sanitizer binaries.
>   - Fix later (shared file): give asan its own build dir, or make memcheck depend on `clean`.
>   - `asan: clean all test` also relies on serial order; it worked with `-j8` here, but that is not guaranteed.
>
> ### Checked and fine
> - **Builds:** compiles cleanly under `-std=c17 -Wall -Wextra -Werror -pedantic`. No VLAs and no heap. Every function has a comment label. The test refuses to build with NDEBUG.
> - **memcheck exit status:** a failure is reported whether the first or the second binary fails (both runs exited 2).
> - **Makefile dependencies:** the new target lists both .c files and both headers. `test` runs both binaries, and a failure in the first stops the run. asan's target-specific CFLAGS reach the new target.
> - **Wrapper:** argument checks happen before the backend is called, and the mock's mismatch flag stays clear. `read`/`write`/`wait` after close return INVALID_ARGUMENT. `os_errno` from the mock reaches the caller unchanged for read, write and wait.
> - **Mock:**
>   - `memcmp`/`memcpy` stay within the borrowed lengths.
>   - A failed read leaves the buffer untouched.
>   - Mismatch is sticky, and `mismatch_step` records the first bad step.
>   - Close fails on unconsumed steps or any earlier mismatch.
>   - A NULL/0 script is allowed.
>   - `is_simulated` is set to 1, and the test output is labeled SIMULATED.
> - **Design:** separate STOP-terminated write and read with no `I2C_RDWR` fits the BH1750 and seesaw timing.
> - **Future Linux backend:** fd ownership (device owns it, drivers borrow) fits the interface. EINTR on nanosleep fits behind `wait_us` once S4 is settled.
