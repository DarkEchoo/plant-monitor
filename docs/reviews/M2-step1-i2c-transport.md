# M2 Step 1 review: I2C transport interface and scripted mock

| Item | Status |
|---|---|
| Author | Jesse (`src/sensors/`, branch `jesse/sensor-capture`) |
| Milestone | M2 (TP-D2) |
| Fresh-context agent review | Done 2026-10-19; findings in [M2-step1-agent-review.md](M2-step1-agent-review.md) |
| **Michael's human review** | **PENDING. Not yet reviewed or approved.** |
| Commit | Not committed. Will use `M2: ...` after Michael signs off. |

## Michael: please look hardest at the Makefile (shared)

### What changed (additive only)

- New variables `TRANSPORT_TEST_TARGET`, `TRANSPORT_SRCS`, `TRANSPORT_HDRS`.
- A new rule builds `build/test_i2c_transport` from the test file and both
  transport sources.
  - It depends on both headers, so a header edit triggers a rebuild.
- `test` builds and runs both test binaries.
  - If `test_analysis` fails, the transport test does not run.
- `asan` (`clean all test`) picks up the sanitizer flags for the new target
  through `test`.
- `memcheck` runs Valgrind on both binaries, joined with `&&`.
  - A failure in either one fails the target.
- `all` is unchanged and still builds only `plant-monitor`.
  - The transport is compiled only by test/asan/memcheck until a driver is
    linked into the product.

No existing rule's flags or behaviour changed.

### N6: stale sanitizer binaries (pre-existing pattern, needs your decision)

**Problem.** Changing CFLAGS doesn't trigger a rebuild, and `asan` writes its
instrumented binaries into the same `build/` directory as the normal build.

After `make asan`:
- `make memcheck` runs Valgrind on ASan binaries and fails. Reproduced on a
  scratch copy:
  `ASan runtime does not come first in initial library list` and
  `make: *** [Makefile:34: memcheck] Error 1`.
- `make test` silently reuses the sanitizer binaries.

**Current workaround (what we do today).** Always run `make clean` between
`make asan` and `make memcheck`. The verification order is:

```sh
make && make test && make asan && make clean && make memcheck
```

**Proposed fix.** This is not applied to the shared Makefile. It was trialled
on a scratch copy only. Build sanitizer binaries in their own directory
through a sub-make:

```make
ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g

asan:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/asan CFLAGS="$(CFLAGS) $(ASAN_FLAGS)" all test
```

This replaces the current two `asan:` lines:

```make
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
asan: clean all test
```

What happened on the scratch copy:
- `make asan` built into `build/asan/` and both tests passed.
- `make memcheck` straight afterwards, with no clean, built normal binaries in
  `build/` and passed with 0 errors.
- `make test` used the normal binaries.
- `make clean` still removes everything, since `build/asan` sits inside
  `build/`.
- `asan` no longer relies on `clean` running before `all` in serial order.

Trade-off: `asan` no longer wipes `build/` first. Sanitizer binaries are
rebuilt only when their sources change, the same as normal builds.

## What Step 1 adds

Step 1 adds no sensor drivers, no Linux backend, and no analysis.

### `include/sensors/i2c_transport.h`, `src/sensors/i2c_transport.c`

- **Device.** A device is an ops table, a context, a 7-bit address and an
  `is_simulated` flag.
- **Transactions.** Each write or read is one STOP-terminated transaction.
  - There is no combined I2C_RDWR operation. The BH1750 needs a STOP after
    each opcode, and the seesaw needs a delay between the register write and
    the read.
- **Ownership.** The device owns its backend resource (a file descriptor on
  Linux). Drivers borrow it and never close it.
  - A device starts zeroed, and every backend's open refuses a device that is
    already open (N5).
- **Short transfers.** A backend that reports OK but moved a different number
  of bytes, fewer or more, is reported as `SHORT_TRANSFER` with both counts.
- **Errors.** `OS_ERROR` always carries the nonzero errno of the failed call.
  - Bad arguments are rejected before the backend runs.
  - Wait results always carry 0/0 byte counts.
  - A wait backend must retry `EINTR` with the remaining time and never report
    OK for a partial wait.
- **Close.** The device is marked closed even when the backend reports an
  error, like `close(2)`. A second close returns `INVALID_ARGUMENT`.

### `include/sensors/i2c_mock.h`, `src/sensors/i2c_mock.c`

- A scripted mock that uses no heap.
- It checks write bytes, step order, and minimum waits. Waits are recorded,
  never slept.
- A mismatch is sticky, and close fails if the script was not fully used.
- It refuses to open a device that is already open, and it always marks
  devices simulated.

### `tests/test_i2c_transport.c`

- 20 tests. The binary prints "SIMULATED".
- A test-only "canned" backend reaches wrapper paths that a well-formed mock
  script cannot produce.

### Plant analysis is untouched

`src/analysis.c`, `include/analysis.h`, `src/main.c` and
`tests/test_analysis.c` are unchanged. There is no light accumulation,
moisture trend, calibration, fusion or care-advice logic.

## Final verification

Run 2026-10-19 on Ubuntu with GCC 15.2.0, GNU Make 4.4.1 and Valgrind 3.26.0,
in this order:

| Command | Exit | Result |
|---|---|---|
| `make` | 0 | builds `build/plant-monitor` |
| `make test` | 0 | `analysis tests passed`; `SIMULATED i2c transport tests passed` |
| `make asan` | 0 | both tests pass under ASan and UBSan, with no sanitizer reports |
| `make clean` | 0 | `rm -rf build` |
| `make memcheck` | 0 | both binaries: `ERROR SUMMARY: 0 errors from 0 contexts`, `All heap blocks were freed` |

## Michael's review

- [ ] Makefile additions reviewed
- [ ] N6 decision: adopt the proposed `build/asan` fix, choose another fix, or
      keep the clean-before-memcheck workaround
- [ ] Transport and mock contract reviewed (ownership, short transfers,
      errors, close, reopen refusal)
- [ ] Approved for an `M2:` commit

Reviewer notes:

_(pending)_
