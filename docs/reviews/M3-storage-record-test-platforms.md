# M3 storage record: test results by platform

| Item | Status |
|---|---|
| Author | Michael (`src/storage/`, branch `michael/storage-record`, stacked on `michael/storage-design` at `b540816`) |
| Scope | Fixed 48-byte record encode/decode, CRC-32, and magic and version checks only |
| Tested tree | Uncommitted working tree from Michael's Mac, copied to `/tmp/storage-record-test.Vu3XpR` on the Pi (44 files, SHA-256 identical to the Mac copy). `~/plant-monitor` on the Pi was not changed. |
| Date | 2026-10-26 |

## Results

| Check | Platform | Result |
|---|---|---|
| `make clean`, `make`, `make test` | Raspberry Pi 3B+, Debian, kernel 6.18.50+rpt-rpi-v8, GCC 14.2.0 | Pass, exit 0. All 7 test binaries pass, including `test_storage_record`. |
| `make memcheck` | Same Pi, Valgrind 3.24.0 | Pass, exit 0. Every binary reports `0 errors from 0 contexts` and `All heap blocks were freed`. |
| ASan + UBSan build of `test_storage_record` (`make asan` flags) | macOS (Darwin 24.6.0), Apple clang 17.0.0 | Pass, exit 0. |
| `make asan` | Same Pi, GCC 14.2.0 and clang 19.1.7 | Fails, exit 2. AddressSanitizer crashes at startup. See below. |

On macOS, the full `make` and `make test` targets do not build, because `src/sensors/i2c_linux.c` and `tests/test_sensor_diag.c` need Linux-only headers and errno values. That was already true before this branch.

## AddressSanitizer on the Pi

Every AddressSanitizer binary stops before `main` with:

```
AddressSanitizer: CHECK failed: sanitizer_allocator_primary64.h:131 "((kSpaceBeg)) == ((address_range.Init(TotalSpaceSize, PrimaryAllocatorName, kSpaceBeg)))" (0x500000000000, 0xfffffffffffffff4)
    <empty stack>
```

With clang the message is the same, except it reports line 133.

- The stack is empty, so no project code has run yet. The first binary to fail is `test_analysis`, which this branch does not change.
- A trivial `int main(void){return 0;}` built with `cc -fsanitize=address` fails with the same message.
- The Pi kernel is configured with `CONFIG_ARM64_VA_BITS=39` (`CONFIG_PGTABLE_LEVELS=3`, 4 KiB pages). The failing address, `0x500000000000`, is beyond what a 39-bit address space can reach. We think this is the cause, but we have not confirmed it.

This is a limitation of the platform or the sanitizer runtime, not a failure in the storage record code. As extra evidence, a one-off build of `test_storage_record` with `-fsanitize=undefined` alone passes on the Pi (exit 0). That build is not the `make asan` target.
