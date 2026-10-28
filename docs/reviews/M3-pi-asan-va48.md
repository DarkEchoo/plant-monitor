# M3: AddressSanitizer on the Pi with a 48-bit VA kernel

| Item | Status |
|---|---|
| Change | Jesse built and installed a custom Raspberry Pi kernel with a 48-bit virtual address space (previously 39-bit) |
| Verified | 2026-10-27, on the project Pi over SSH, with Claude Code in Jesse's session |
| Tested commit | `main` at `6fef78bc9648a245aa0c1ea9528e03433197df85`, in a fresh clone in `/tmp` (the existing `~/plant-monitor` checkout and its `build/` were not touched) |
| Result | **`make asan` now passes on the Pi**, with GCC and Clang. The startup failure recorded in `M3-storage-record-test-platforms.md` no longer occurs on this kernel |

## The earlier failure

`M3-storage-record-test-platforms.md` records that on the stock kernel (`6.18.50+rpt-rpi-v8`, `CONFIG_ARM64_VA_BITS=39`), every AddressSanitizer binary stopped before `main`:

```
AddressSanitizer: CHECK failed: sanitizer_allocator_primary64.h:131 ... (0x500000000000, 0xfffffffffffffff4)
```

That happened under both GCC 14.2.0 and Clang 19.1.7, and also for a trivial program. The note suspected the 39-bit address space, since the runtime's fixed allocator address `0x500000000000` is out of its reach, but left that unconfirmed.

## Platform now

| Item | Value |
|---|---|
| Board | Raspberry Pi 3 Model B Plus Rev 1.4 |
| Kernel | `Linux 6.18.50-plantmonitor-va48 #1 SMP PREEMPT Tue Oct 27 20:07:59 CDT 2026 aarch64` |
| Kernel config | `CONFIG_ARM64_VA_BITS_48=y`, `CONFIG_ARM64_VA_BITS=48`, `CONFIG_PGTABLE_LEVELS=4`, `CONFIG_ARM64_4K_PAGES=y` |
| Page size | 4096 |
| Address space | The stack is mapped at `fffff0206000-fffff0227000`, an address that only exists with 48-bit addressing |
| OS | Debian GNU/Linux 13 (trixie), `arm64`, `LONG_BIT=64` |
| Tools | GCC 14.2.0 (Debian 14.2.0-19), Clang 19.1.7, Valgrind 3.24.0, GNU Make 4.4.1 |

The compiler versions are the same as in the failing run.

## Checks run on the Pi

| Check | Command | Result |
|---|---|---|
| Trivial program | `gcc`/`clang -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g` on `int main(void){return 0;}` | Both exit 0 (no startup failure) |
| ASan is active | The same flags on a deliberate 1-byte heap overflow | Both exit 1 with `ERROR: AddressSanitizer: heap-buffer-overflow` |
| UBSan is active | The same flags on a deliberate signed overflow | Both exit 1 with `runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'` |
| Full target, GCC | `make asan` | **Exit 0.** All 12 test binaries pass (13 pass lines; the Linux backend test prints two), with 0 sanitizer reports |
| Full target, Clang | `make CC=clang BUILD_DIR=build-clang asan` | **Exit 0.** All 13 pass lines, 0 sanitizer reports |
| Tests and Valgrind | `make BUILD_DIR=build-plain test memcheck` | Exit 0. All 12 binaries pass, and Valgrind reports `ERROR SUMMARY: 0 errors` and `All heap blocks were freed` for 12/12 |

`test_capture_linux` printed `clock trust on this machine: TRUSTED` on the Pi during this run. That's an observation only; the test doesn't check it.

The deliberate-bug probes matter: they show the sanitizers are active on the Pi, not that the build simply stopped failing.

## What this shows, and what it doesn't

- **Fixed on this Pi:** with the 48-bit VA kernel, `make asan` runs to completion and passes with both compilers. The Pi can now produce full ASan/UBSan evidence instead of relying on desktop `make asan` plus Pi Valgrind.
- **Cause:** the toolchain versions are the same as the failing run, and moving the address space from 39 to 48 bits removed the failure. That strongly supports the 39-bit address space as the cause. This custom kernel's other configuration differences from the stock kernel were not compared.
- **Not covered:** no sensors or I2C bus were used. All tests are mocked or simulated, as on the desktop. This is not a hardware acceptance result.

## Open item

**The project Pi now runs a custom kernel.** To keep later results (the hardware run, the 48-hour soak) reproducible, the kernel's source, configuration and build steps should be recorded. They aren't recorded yet. It's also worth confirming that I2C (`/dev/i2c-1`) behaves the same on this kernel; Part A of `docs/hardware-acceptance.md` covers that once the sensors are connected.
