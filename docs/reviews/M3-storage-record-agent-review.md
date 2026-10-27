# M3 storage record: fresh-context agent review findings

- **Reviewer:** a fresh-context Claude Code subagent (general-purpose) with
  read-only instructions. It started with no context from the authoring
  session.
- **Date:** 2026-10-26.
- **Scope:** PR #9, `b540816..d3c26c8`: `Makefile`, `include/storage/crc32.h`,
  `include/storage/record.h`, `src/storage/crc32.c`, `src/storage/record.c`,
  `tests/test_storage_record.c`,
  [M3-storage-record-test-platforms.md](M3-storage-record-test-platforms.md).
- **Verdict:** no blockers; 1 substantive finding and 3 nits, all fixed in
  tests and documentation. No implementation change was needed.
- **Human review:** Jesse's review of PR #9 is **PENDING**. This agent review
  does not replace it.

## What the reviewer verified

- **Layout:** every offset and width in `record.c` matches the design.md
  table. An independent Python `struct.pack('<BBHQQQIBBHI', ...)` of the
  test's field values equals the hand-written bytes 0-43.
- **CRC:** `zlib.crc32` of bytes 0-43 is `0x990f76c6`, and
  `zlib.crc32(b'123456789')` is `0xcbf43926`.
- **Failure paths:** decode writes nothing to `*out` on any failure.
- **Code safety:** no undefined behavior, no VLAs, and no dependence on
  struct padding.
- **Build:** the test passed on macOS in a scratch copy of `d3c26c8`, both
  normally and with ASan + UBSan.

## How each finding was handled

| # | Severity | Finding | Resolution |
|---|---|---|---|
| S1 | substantive | The tests did not enforce the documented check order (magic, then version, then CRC). Checking the CRC before the version, or the version before the magic, still passed the whole suite. | **Fixed.** `test_bad_version` now expects `BAD_VERSION` while the CRC is stale, before resealing. `test_bad_magic` now expects `BAD_MAGIC` for a bad magic plus a bad version, with and without a valid CRC. See the mutation check below. |
| N1 | nit | The golden-CRC comment used a placeholder, so it could not be rerun. | **Fixed.** The comment now has the full 44 bytes as `bytes.fromhex(...)`. Running it exactly as written prints `0x990f76c6`. |
| N2 | nit | The platform note did not tie its results to a commit. | **Fixed.** The note now says the 44 tested files match `d3c26c8` by SHA-256. This was checked by comparing a `git archive d3c26c8` against the checksums taken before the Pi copy. |
| N3 | nit | The NULL-encode test checked only `buf[0]` and `buf[47]`. | **Fixed.** It now compares all 48 bytes against an untouched 0xAA-filled copy. |

## Mutation check for S1

Two copies of `record.c` with the checks swapped were built in a scratch
directory, outside the repo. Each copy was tested against the test file from
`d3c26c8` and against the updated test file.

| Swapped order | Test from `d3c26c8` | Updated test |
|---|---|---|
| CRC checked before version | exit 0 (missed) | exit 134, `test_bad_version`: expected `BAD_VERSION` |
| Version checked before magic | exit 0 (missed) | exit 134, `test_bad_magic`: expected `BAD_MAGIC` |

The updated test passes against the real `record.c` (exit 0).

## Retest after the fixes

- **macOS:** the 5 tests that build on macOS pass, and `test_storage_record`
  also passes with ASan + UBSan.
- **Raspberry Pi:** in a fresh temp copy (`/tmp/storage-record-review.YwMRvQ`,
  with `tests/test_storage_record.c` SHA-256 matching the Mac copy),
  `make clean`, `make`, `make test`, and `make memcheck` all exit 0. All 7
  test binaries pass, and each reports `ERROR SUMMARY: 0 errors` under
  Valgrind. `make asan` was not rerun: AddressSanitizer cannot start on this
  kernel (see
  [M3-storage-record-test-platforms.md](M3-storage-record-test-platforms.md)).
