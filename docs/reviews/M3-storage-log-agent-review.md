# M3 storage log writer: fresh-context agent review findings

- **Reviewer:** a fresh-context Claude Code subagent (general-purpose) with
  read-only instructions. It started with no context from the authoring
  session.
- **Date:** 2026-10-26.
- **Scope:** PR #10, commit `d43ea3d`: `Makefile`,
  `include/storage/log_writer.h`, `src/storage/log_writer.c`,
  `src/storage/log_writer_sys.h`, `tests/test_storage_log.c`.
- **Verdict:** no high-severity findings. 3 medium, 6 low, and 3 nits.
  Findings 1-4 and 9 are fixed in this change. The rest are deferred and
  listed below.
- **Human review:** Jesse's review of PR #10 is **PENDING**. This agent
  review does not replace it.

## What the reviewer verified

- **Bounds:** the batch buffer stays in bounds. `file_size_bytes` counts
  only bytes the kernel accepted, including after a partial write that then
  fails.
- **Writes:** a short write resumes at the right offset, and `EINTR` on
  write is retried.
- **Failures:** a failed write or `fdatasync` is never retried. Close
  always releases the descriptor and never retries `close()`.
- **Open:** the steps run in order (open directory, `openat`, `fstat`,
  alignment check, `fsync` of the directory, close of the directory), and
  every failure path closes exactly what it opened.
- **Build:** clean under C17 `-Wall -Wextra -Werror -pedantic`. The test
  passed on macOS, both normally and with ASan + UBSan.

## How each finding was handled

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | medium | A zero-initialized writer has `fd == 0` and `sys == NULL`, so it counted as open. Append buffered a record, and close crashed on a NULL `sys` (reproduced). | **Fixed.** New `is_open` field. All zeros means closed, and fd 0 is now a valid log descriptor. Tests: `test_file_zero_writer_not_open`, `test_sim_fd_zero_is_open`. |
| 2 | medium | Opening an open writer leaked the old descriptor and dropped its buffered records (reproduced). | **Fixed.** New `STORAGE_LOG_ALREADY_OPEN`, returned before any system call, with the writer unchanged. Tests: `test_file_open_twice_refused`, `test_sim_open_twice_no_calls`. |
| 3 | medium | Simulated and live records were kept apart only within one open. A live file could be reopened as simulated and given SIMULATED records (reproduced). | **Fixed.** The name must start with `SIMULATED-` exactly when the writer is simulated; this is checked before any system call. A non-empty file's last record is read with `pread`, and its SIMULATED flag must match. An unreadable last record or an unknown type gives the new `STORAGE_LOG_BAD_TAIL`. A `pread` error gives `OS_ERROR` at the new stage `READ_TAIL`. The full `plant-<16 hex>.log` format is left to the rotation change. Tests: `test_sim_name_prefix`, `test_file_reopen_checks_tail`, `test_sim_tail_read`. |
| 4 | low | `next_sequence` was not checked against an existing file, so a reopen could duplicate sequence numbers (reproduced). | **Fixed** by the same last-record check: `next_sequence` must be one past the last record's sequence, and a last sequence of `UINT64_MAX` is refused. Tests: `test_file_reopen_checks_tail`, `test_sim_tail_read`. |
| 5 | low | The header said `w->fd` is -1 after any failed open, but an `ARGUMENT` failure leaves it unchanged. | **Fixed in the header only.** It now says `*w` is unchanged on `ARGUMENT` and `ALREADY_OPEN`. The code was not changed, because resetting the fd there would be wrong for an open writer. |
| 6 | low | When the batch-full flush fails inside append, the record was already accepted, but the result looks like a rejection. | **Deferred.** To be documented with the hub, which logs the lost sequence range. |
| 7 | low | `next_sequence` wraps after `UINT64_MAX`. | **Partly fixed:** a reopen after a last record of `UINT64_MAX` is refused (finding 4). Append itself still does not refuse the wrap; deferred, since it cannot happen in practice. |
| 8 | low | Append does not enforce design.md "Failure detail" zero-field rules (bytes 36-39 on VALID records, 32-39 on START). | **Deferred.** Whether the writer or the hub owns these checks is a team decision. |
| 9 | low | Only `flush_if_due` enforced the 60 s limit. An append arriving more than 60 s after the oldest buffered record did not flush. | **Fixed.** Checked against design.md "Batching and fsync", which says records are written "at least every 60 seconds". Append now flushes when the oldest buffered record is due. The writer has no timer, so the header says the caller must still call `flush_if_due` when no records arrive. `now < oldest` is still treated as not due. Tests: `test_file_append_flushes_overdue`, `test_sim_append_overdue_failure`. |
| 10 | nit | On a directory-close failure, open returns inline instead of through a cleanup label. | **Deferred.** The code is correct. |
| 11 | nit | Some test-fake fields lack units in their names. | **Deferred.** The new fields (`pread_len_bytes`) have units. |
| 12 | nit | `EINTR` from open, `fsync`, and `fdatasync` is a hard error, and this is not documented. | **Deferred.** `pread` of the last record now retries `EINTR`, like write. |

## Bug found while fixing finding 3

The log file was opened `O_WRONLY`, so the new `pread` failed with `EBADF`.
The fake syscall table could not show this. The real-file test
`test_file_reopen_appends` caught it. The file is now opened
`O_RDWR | O_APPEND`; `O_APPEND` still sends every write to the end.

## Tests first

The new tests were added before the fixes, together with the interface
changes they need (`is_open`, new statuses and stage, `pread` in the
syscall seam). Each test was then run on its own against the unfixed
writer:

- **Still passing:** all 17 existing tests.
- **Failing:** all 9 new tests, each on an assertion:
  - `test_file_zero_writer_not_open`
  - `test_file_open_twice_refused`
  - `test_file_reopen_checks_tail`
  - `test_file_append_flushes_overdue`
  - `test_sim_fd_zero_is_open`
  - `test_sim_open_twice_no_calls`
  - `test_sim_name_prefix`
  - `test_sim_tail_read`
  - `test_sim_append_overdue_failure`

**Changes to existing tests:**

- Two FILE tests that open in simulated mode now use `SIMULATED-` file
  names (`SIMULATED-b.log`, `SIMULATED-i.log`), as the new rule requires.
- `test_sim_open_sequence` and `test_sim_open_failures` gained assertions
  that no `pread` happens for an empty or misaligned file.
- No assertion was removed or loosened.

## Mutation check

Copies of `log_writer.c`, each with one check broken, were built in a
scratch directory outside the repo and run against the updated test.

| Broken check | Result |
|---|---|
| `check_open` tests `fd < 0` instead of `is_open` | caught, exit 134 |
| close tests `fd < 0` instead of `is_open` | caught, exit 139 (SEGV on NULL `sys`) |
| no already-open guard | caught, exit 134 |
| no name prefix check | caught, exit 134 |
| no SIMULATED check on the last record | caught, exit 134 |
| last record not checked at all | caught, exit 134 |
| last-record `pread` not retried on `EINTR` | caught, exit 134 |
| short last-record read accepted | caught, exit 134 (see note) |
| last record's type not checked | caught, exit 134 |
| no sequence check on the last record | caught, exit 134 |
| last sequence `UINT64_MAX` not refused | caught, exit 134 |
| append ignores a due batch | caught, exit 134 |
| 60 s deadline off by one | caught, exit 134 |

The 7 mutants from the original change (no `EINTR` retry on write,
non-sticky write failure, flush at 31, no directory fsync, directory fd
leaked on open failure, no SIMULATED check in append, close skipping the
flush) are all still caught.

Note: the short-read mutant was first missed. The fake copied only 47
bytes, so the decode failed on the missing CRC byte by chance. The fake now
fills the whole record but reports 47 bytes, so only the length check can
refuse it.

## Retest after the fixes

- **macOS:** `test_storage_log` passes, both normally and with ASan +
  UBSan. The full `make` does not build on macOS (Linux-only I2C headers),
  which was already true.
- **Raspberry Pi 3B+** (Linux 6.18.50+rpt-rpi-v8 aarch64, GCC 14.2.0,
  Valgrind 3.24.0): the final tree was copied to a fresh
  `/tmp/storage-log-review.MLSzXC`. SHA-256 of `Makefile` and the four
  log writer files matches the Mac copy. Results:
  - `make clean`, `make`, `make test`, and `make memcheck` all exit 0.
  - All 8 test binaries pass, and each reports `ERROR SUMMARY: 0 errors`
    and `All heap blocks were freed` under Valgrind.
  - `make asan` exits 2 with the known AddressSanitizer startup crash (see
    [M3-storage-record-test-platforms.md](M3-storage-record-test-platforms.md)).
  - A UBSan-only build of `test_storage_log` passes, exit 0.
