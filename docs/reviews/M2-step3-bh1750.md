# M2 Step 3 review: BH1750 light-sensor driver

| Item | Status |
|---|---|
| Author | Jesse (`src/sensors/`, branch `jesse/bh1750`, based on `becdd82` from `jesse/i2c-linux`) |
| Milestone | M2 (TP-D2) |
| Depends on | PR #2 (Step 2), which depends on PR #1. All are unmerged drafts. |
| Fresh-context agent review | Done 2026-10-21. No blocking issues; 1 substantive and 3 nits, all fixed (below). |
| **Michael's human review** | **PENDING. Not reviewed or approved.** |
| Commit | `M2: add BH1750 one-time measurement driver`, pushed to `jesse/bh1750` as a draft PR based on `jesse/i2c-linux`. |
| Hardware | **None used.** No BH1750, Raspberry Pi or I2C bus was touched. |

## Source document

- **Cited (read):** ROHM, *BH1750FVI Technical Note*, **Rev.C, 2010.04**, 17 pages.
  - Obtained from a distributor mirror: <https://cdn.velleman.eu/downloads/29/infosheets/bh1750fvi-e_datasheet.pdf>.
  - SHA-256 of the PDF read: `1ea9c79710cfbe2a5cce59945f4f08aba2da452eaf2c1441ada56946ce388fee`.
  - The PDF is not committed (copyright).
  - rohm.com's product page returned 404, and distributors list the part as not for new designs.
- **Not read:** Rev.D, 2011.11, hosted at Mouser. The fetch timed out twice. **No specification in this step comes from Rev.D.**

| Fact used | Rev.C text | Where used |
|---|---|---|
| Power On | "Power On 0000_0001 Waiting for measurement command." | `BH1750_CMD_POWER_ON 0x01` |
| One Time H-Resolution Mode | "0010_0000 … Start measurement at 1lx resolution. … It is automatically set to Power Down mode after measurement." | `BH1750_CMD_ONE_TIME_H_RES 0x20` |
| MTreg default | "Measurement Time Register … Initial Value is "0100_0101"." and "69 : Default value of MTreg ( dec )" | MTreg = 69 |
| MTreg commands | "Change Measurement time 01000_MT[7,6,5]" and "011_MT[4,3,2,1,0]". The example writes each as its own `ST … SP` transaction. | `0x42` = `01000_010`, `0x65` = `011_00101` |
| Encoding check | The example sets MTreg `1000_1010` with "01000_100" and "011_01010", which matches the same bit split | — |
| What resets registers | "Asynchronous reset: All registers are reset." (DVI = 'L'). "Reset command is for only reset Illuminance data register." | Power On is **not** documented to restore MTreg, so init sets it explicitly |
| H-res timing | Electrical Characteristics: "tHR － 120 180 ms", specified at "VCC = 3.0V, DVI = 3.0V, Ta = 25℃" | `BH1750_H_RES_MAX_WAIT_US 180000u` |
| One opcode per transaction | "BH1750FVI is not able to accept plural command without stop condition. Please insert SP every 1 Opecode." | each command is its own STOP-terminated write |
| Byte order | "High Byte [ 15:8 ]" is read before "Low Byte [ 7:0 ]" | `raw = bytes[0] << 8 \| bytes[1]` |
| Conversion | "Measurement Accuracy S/A 0.96 1.2 1.44 times", "( 2^15 + 2^9 + 2^8 + 2^7 + 2^4 ) / 1.2 ≒ 28067 [ lx ]", "( 2^8 + 2^4 ) / 1.2 ≒ 227 [ lx ]", and "Illuminance per 1 count … = 1 / 1.2 *( 69 / X )" | `mlx = counts * 2500 / 3` |
| Addresses | "ADDR = 'H' ( ADDR ≧ 0.7VCC ) → "1011100"" and "ADDR = 'L' ( ADDR ≦ 0.3VCC ) → "0100011"" | only 0x5C and 0x23 accepted |
| Power On optional | "\"Power On\" Command is possible to omit." | Sent anyway: init is explicit, and the write is an early presence check |

## API: `include/sensors/bh1750.h`

```c
bh1750_status bh1750_init(i2c_device *dev);
bh1750_status bh1750_measure_once_h(i2c_device *dev, bh1750_reading *out);
uint32_t bh1750_raw_to_mlx(uint16_t raw_count);
const char *bh1750_stage_name(bh1750_stage stage);
```

- **Stateless.** Every function borrows an already-open `i2c_device` and never opens or closes it.
- **Preconditions, documented in the header:**
  - `bh1750_init` must succeed before measuring, and again after the sensor loses power.
  - The driver needs exclusive use of the sensor's address.
- **Accepted addresses.** Only 0x23 and 0x5C, each the datasheet's own encoding. Anything else is `STAGE_ARGUMENT` with no I/O. This keeps BH1750 opcodes away from, say, the soil sensor at 0x36.
- **Init** sends three separate STOP-terminated writes: Power On `0x01`, MTreg high `0x42`, MTreg low `0x65`. It stops at the first write that is not fully OK and reports `POWER_ON`, `MTREG_HIGH` or `MTREG_LOW`.
- **Measure** writes `0x20`, waits 180 000 µs through `i2c_device_wait_us`, then reads 2 bytes.
  - It stops at the first failure (`MEASURE_COMMAND`, `WAIT` or `READ`) and returns that step's `i2c_result` unchanged.
  - `*out` is written only after a complete 2-byte read with status OK. A short read leaves it untouched even if one byte was copied into the driver's local buffer.
- **Reading fields.**
  - `raw_count`: sensor counts.
  - `illuminance_mlx`: millilux.
  - `is_simulated`: copied from the device, so mock readings stay labelled.
  - There are no timestamps, because the capture layer will stamp readings.
- **Conversion.** `mlx = (counts * 2500 + 1) / 3`, computed in `uint32_t`.
  - **Rounding:** round to nearest. A remainder of 0, 1 or 2 out of 3 means a fraction of 0, 1/3 or 2/3, so there are no ties to break.
  - **Overflow:** the largest intermediate is 65535 × 2500 + 1 = 163 837 501, below 2^32. Casting to `uint32_t` before the multiply also keeps this correct where `int` is 16 bits.
  - **Worked examples:** 33680 counts gives 28 066 667 mlx, which is 28067 lx to the nearest lux, matching the datasheet's "≒ 28067". 272 counts gives 226 667 mlx, which is 227 lx.

## Tests: `tests/test_bh1750.c` (SIMULATED, scripted mock)

| Test | Covers |
|---|---|
| `test_raw_to_mlx_vectors` | 0 → 0, 1 → 833, 65535 → 54 612 500 (boundaries); 2 → 1667, 3 → 2500 (rounding); the H-resolution worked example (33680 → 28067 lx); the L-resolution example (272 → 227 lx), which checks the /1.2 arithmetic only |
| `test_init_sends_documented_commands` | exactly `0x01`, `0x42`, `0x65`, in order, with no wait |
| `test_init_stops_at_each_failure` | each of the 3 commands with `EIO` and with a 0-byte short write. The right stage and status come back, and nothing after it is sent: the script ends at the failing step, so any extra write would fail close. |
| `test_measure_example` | `0x20`, a wait of at least 180 000 µs (a shorter wait is a mock mismatch), and read `{0x83,0x90}` → 33680 / 28 066 667 mlx / simulated |
| `test_byte_order` | `{0x01,0x10}` → 272, not 4097 |
| `test_init_then_measure` | the full sequence at 0x5C |
| `test_measure_stops_at_each_failure` | command `EIO` and short write; wait `EINVAL`; read `ENXIO`; 1-of-2 short read; 0-byte read. The right stage comes back, the output sentinel is unchanged, and no later step runs. |
| `test_arguments_rejected` | NULL dev; a zeroed device; **a device closed after being open at 0x23**; NULL out; addresses 0x00, 0x22, 0x24, 0x36, 0x5B, 0x5D, 0x7F. Each is `STAGE_ARGUMENT` with no steps consumed and the output unchanged. |
| `test_simulation_label_copied` | `is_simulated` is copied from the device, not hard-coded |
| `test_stage_names` | stable log text, including `UNKNOWN` for an out-of-range stage |

## Tests-first evidence

1. With only the header and tests written, `make test` stopped with `No rule to make target 'src/sensors/bh1750.c'`.
2. Against a throwaway stub (scratchpad only, never in the repo), the tests compiled and linked, then failed:
   `test_raw_to_mlx_vectors: Assertion 'bh1750_raw_to_mlx(vectors[i].raw) == vectors[i].mlx' failed` (exit 134).

## Mutation checks (scratch copies, 60 s timeout each; all 18 caught after review)

| Broken guard | Caught by |
|---|---|
| continuous-mode opcode `0x10` instead of `0x20` | `test_measure_example` |
| MTreg high `0x44`; skip MTreg low; skip Power On | `test_init_sends_documented_commands` |
| continue after an init failure; wrong init stage | `test_init_stops_at_each_failure` |
| wait the 120 ms typical instead of the 180 ms max | `test_measure_example` (mock minimum wait) |
| continue after a wait failure; accept a short read | `expect_measure_failure` |
| little-endian decode | `test_measure_example` |
| truncating division; round half up (`+2`); 16-bit intermediate | `test_raw_to_mlx_vectors` |
| no address check | `test_arguments_rejected` |
| hard-coded `is_simulated = 1` | `test_simulation_label_copied` |
| no NULL `out` check | `test_arguments_rejected` |
| no `ops != NULL` check (review S1; survived before the fix) | `test_arguments_rejected` (closed at 0x23) |
| `UNKNOWN` default changed (review N1; survived before the fix) | `test_stage_names` |

## Final verification

Run 2026-10-21 on Ubuntu with GCC 15.2.0, GNU Make 4.4.1 and Valgrind 3.26.0, in this order:

| Command | Exit | Result |
|---|---|---|
| `make clean` | 0 | `rm -rf build` |
| `make` | 0 | builds `build/plant-monitor` (unchanged; the driver is not linked into the product yet) |
| `make test` | 0 | all five pass lines, including `SIMULATED bh1750 driver tests passed (scripted mock; no sensor hardware used)` |
| `make asan` | 0 | `build/asan/`: all suites pass, with no ASan/UBSan reports |
| `make memcheck` | 0 | straight after `asan`, no clean: all four binaries report `ERROR SUMMARY: 0 errors from 0 contexts` and `All heap blocks were freed` |
| `make test` | 0 | all pass |

Also, `make clean && make -j8 asan` followed by `make -j8 memcheck` with no clean passed.

**Unchanged:** the analysis files, transport, mock and Linux backend.

## Not verified / accepted limitations

- **No hardware.** No real BH1750 transaction has happened, and `i2cdetect` has not been run. The Adafruit board's default address is not yet confirmed on the device.
- **MTreg retention.** Rev.C does not say whether MTreg survives the automatic Power Down after a one-time measurement. The driver assumes it does. This must be confirmed on hardware, for example by checking that repeated measurements stay consistent after one init.
- **Accuracy.** 1.2 is the *typical* factor. The datasheet range is 0.96 to 1.44 ("Small measurement variation (+/- 20%)"). Dark output can read 0 to 3 counts. This is a measurement-accuracy limit, not a calibration, and any interpretation belongs to later analysis steps.
- **Timing conditions.** The 180 ms maximum is specified at VCC = 3.0 V, DVI = 3.0 V and Ta = 25 ℃. The board runs at 3.3 V and may see other temperatures, and the note gives no figure for those. Jesse decided (2026-10-21) to keep exactly the documented 180 ms maximum, with no undocumented margin. Hardware testing should check for readings that are stale or zero.
- **Rev.D** was not read.
- **Exclusive ownership** is documented, not enforced. The non-forced `I2C_SLAVE` stops a kernel driver from sharing the address, but not another user-space process.

## Fresh-context review

The reviewer was a fresh-context agent, given read-only instructions and the extracted Rev.C text. It did not use the network.

It independently checked every hardware fact against Rev.C: the opcodes, the MTreg default, the `0x42`/`0x65` encoding (derived again, and cross-checked with the note's own `1000_1010` example), the absence of any Power On restore statement, tHR, the byte order, the /1.2 factor, both worked examples and both addresses. **All match.**

It verified by running code:
- `make test`/`asan`/`memcheck` pass.
- `bh1750.c` builds clean under `-Wvla -Wconversion -Wsign-conversion -Wshadow` and `-O2 -fanalyzer`.
- Of 17 mutants, 14 were killed. The 3 survivors were S1, N1, and one harmless extra switch case.

It did not run a 32-bit or 16-bit-`int` build, because there is no `-m32` libc and no clang. Safety there was checked by reading: the `uint32_t` cast comes before the multiply, and the byte shift has no UB at either `int` width.

| # | Severity | Finding | Resolution |
|---|---|---|---|
| S1 | substantive (test gap) | The "closed device" test used `{0}` (address 0x00), so the address check hid a missing `ops != NULL` check (the mutant survived). | **Fixed.** A device opened at 0x23 and then closed must give `STAGE_ARGUMENT` for init and measure, with the output unchanged. The mutant is now killed. |
| N1 | nit | The `UNKNOWN` stage text was untested. | **Fixed.** The mutant is now killed. |
| N2 | nit | The 180 ms was cited from ex1, which is a *continuous*-mode example. The tHR row is the stronger source, and its conditions are 3.0 V / 25 ℃. | **Fixed.** The header and this doc cite the tHR row and its conditions. Kept at exactly 180 ms, the documented maximum as approved; the difference in conditions is listed under limitations. |
| N3 | nit | The "272" vector comes from the note's L-resolution example. | **Fixed.** The comment now says it checks only the /1.2 arithmetic. |

The reviewer confirmed as fine:
- rounding (no ties)
- overflow bounds
- the 16-bit-`int` promotion
- the transport contract (status and stage propagation, `*out` written only after OK, the wait pinned to 180 000 µs)
- the address restriction and the `is_simulated` copy
- the Makefile wiring
- the documentation of the init requirement and exclusive ownership
- no scope creep

No substantive findings remain.

## Michael's review

- [ ] Datasheet facts and the MTreg = 69 initialisation
- [ ] API (stateless, stages, output-unchanged rule, address restriction)
- [ ] Conversion and rounding
- [ ] Makefile additions (shared)
- [ ] Approved for commit and PR

Reviewer notes: _(pending)_
