# M2 Step 1 review: I2C transport interface and scripted mock

| Item | Status |
|---|---|
| Author | Jesse (`src/sensors/`, branch `jesse/sensor-capture`) |
| Milestone | M2 (TP-D2) |
| Fresh-context agent review | Done 2026-10-19; findings in [M2-step1-agent-review.md](M2-step1-agent-review.md) |
| **Michael's human review** | Changes requested on PR #1 (2026-10-21). Fixes pushed; **re-review PENDING. Not approved.** |
| Commits | `0100a43` M2: add sensor transport abstraction and mocked tests; review fixes in `M2: isolate sanitizer builds and validate mock addresses` |

## Michael's PR #1 review (2026-10-21) and how it was addressed

Michael (`mburns45`) reviewed commit `0100a43` and requested changes. His
comments, quoted exactly:

**Review summary (CHANGES_REQUESTED):**

> This is the main thing we should fix before merging. Right now ASan and
> Valgrind are using the same build directory, which can cause make memcheck
> to fail because it picks up binaries built with ASan. Could we put the ASan
> build in a separate build/asan directory so the two don’t interfere with
> each other?

**Thread 1, `Makefile` line 29
([r4332797783](https://github.com/DarkEchoo/plant-monitor/pull/1#discussion_r4332797783)):**

> I think we should fix this before merging instead of having to remember to
> run make clean between ASan and Valgrind. Could we just use a separate
> build/asan directory so the builds don’t interfere with each other?

- **Addressed.** This is N6 below. Michael chose the proposed `build/asan` fix.
- `asan` now runs a recursive make with `BUILD_DIR=$(BUILD_DIR)/asan` and the
  sanitizer flags, building `all test` there.
- Normal builds, `make test` and `make memcheck` still use `build/`.
- `asan` no longer depends on `clean`, so there is no clean/build race under
  `make -j`.
- `make clean` still removes both, because `build/asan` is inside `build/`.

**Thread 2, `src/sensors/i2c_mock.c` line 111
([r4332806357](https://github.com/DarkEchoo/plant-monitor/pull/1#discussion_r4332806357)):**

> Since this is supposed to be a 7-bit I2C address, should we reject anything
> over 0x7f here and add a quick test for it? That would keep the mock and the
> real backend using the same rules.

Michael's follow-up in the same thread:

> This isn’t an issue more of a suggestion.

- **Adopted** (non-blocking suggestion).
- `i2c_mock_open` returns `INVALID_ARGUMENT` for an address above 0x7f.
  The check sits alongside the existing argument checks, before any field is
  written, so the device and mock are untouched and no steps are consumed.
- `i2c_transport.h` states this as a contract that every backend must follow,
  with `I2C_ADDRESS_7BIT_MAX` (0x7f) defined there. The future Linux backend
  must check it before `open()` or `ioctl(I2C_SLAVE)`.
- Only the range is checked. Reserved addresses inside 0x00-0x7f are not
  refused.
- `test_open_rejects_non_7bit_address` covers the boundary:
  - 0x80 and 0xff are rejected. The zeroed device stays zeroed, and a
    sentinel-filled mock keeps every field.
  - 0x7f opens, and its script runs from step 0.
- The test was written first. Against the old mock it failed with
  `Assertion 'i2c_mock_open(&mock, &dev, bad_addresses[i], ...) ==
  I2C_STATUS_INVALID_ARGUMENT' failed`.

Not done: approving the review, resolving his threads, or merging. Those are
Michael's calls after he re-reviews.

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

### N6: stale sanitizer binaries (resolved after Michael's review)

**Problem.** Changing CFLAGS doesn't trigger a rebuild, and `asan` writes its
instrumented binaries into the same `build/` directory as the normal build.

After `make asan`:
- `make memcheck` runs Valgrind on ASan binaries and fails. Reproduced on a
  scratch copy:
  `ASan runtime does not come first in initial library list` and
  `make: *** [Makefile:34: memcheck] Error 1`.
- `make test` silently reuses the sanitizer binaries.

**Old workaround (no longer needed).** Run `make clean` between
`make asan` and `make memcheck`.

**Fix, applied after Michael chose it on PR #1.** Build sanitizer binaries in
their own directory through a sub-make. The committed version also adds a
one-line comment above the rule:

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

What happened in the final verification below (first seen on a scratch copy):
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
- It refuses an address above 0x7f (added after Michael's review).

### `tests/test_i2c_transport.c`

- 21 tests. The binary prints "SIMULATED".
- A test-only "canned" backend reaches wrapper paths that a well-formed mock
  script cannot produce.

### Plant analysis is untouched

`src/analysis.c`, `include/analysis.h`, `src/main.c` and
`tests/test_analysis.c` are unchanged. There is no light accumulation,
moisture trend, calibration, fusion or care-advice logic.

## Final verification

### After Michael's review fixes

Run 2026-10-21 on Ubuntu with GCC 15.2.0, GNU Make 4.4.1 and Valgrind 3.26.0,
in this order, with no `make clean` between `asan` and `memcheck`:

| Command | Exit | Result |
|---|---|---|
| `make clean` | 0 | `rm -rf build` |
| `make` | 0 | builds `build/plant-monitor` |
| `make test` | 0 | `analysis tests passed`; `SIMULATED i2c transport tests passed` |
| `make asan` | 0 | builds into `build/asan/`; both tests pass under ASan and UBSan with no sanitizer reports |
| `make memcheck` | 0 | uses `build/`: `ERROR SUMMARY: 0 errors from 0 contexts`, `All heap blocks were freed` for both binaries |
| `make test` | 0 | both tests pass with the normal `build/` binaries |

Extra checks:

- `make clean && make -j8 asan` passed. Running it again gave `Nothing to be
  done for 'all'` and both tests passed.
- `make -j8 && make -j8 memcheck` straight afterwards, with no clean, gave 0
  errors for both binaries.
- `ldd` shows `libasan` linked into all three `build/asan/` binaries and none
  of the three `build/` binaries.
- After touching `src/sensors/i2c_mock.c`, `make -j8 asan` rebuilt
  `build/asan/test_i2c_transport` and left the mtimes of the `build/` binaries
  unchanged.

### Before Michael's review (2026-10-19, commit `0100a43`)

| Command | Exit | Result |
|---|---|---|
| `make` | 0 | builds `build/plant-monitor` |
| `make test` | 0 | `analysis tests passed`; `SIMULATED i2c transport tests passed` |
| `make asan` | 0 | both tests pass under ASan and UBSan, with no sanitizer reports |
| `make clean` | 0 | `rm -rf build` |
| `make memcheck` | 0 | both binaries: `ERROR SUMMARY: 0 errors from 0 contexts`, `All heap blocks were freed` |

## Michael's review

- [x] Makefile additions reviewed (PR #1, 2026-10-21; changes requested)
- [x] N6 decision: Michael asked for the `build/asan` fix (applied; awaiting
      his re-review)
- [ ] Re-review of the review fixes (`build/asan`, 7-bit address check)
- [ ] Transport and mock contract reviewed (ownership, short transfers,
      errors, close, reopen refusal)
- [ ] Approved for an `M2:` commit

Reviewer notes:

- 2026-10-21: changes requested on PR #1. See "Michael's PR #1 review" above
  for his exact comments.
- Re-review: _(pending)_
