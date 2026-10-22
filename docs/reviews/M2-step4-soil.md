# M2 Step 4 review: STEMMA soil-sensor driver (seesaw)

| Item | Status |
|---|---|
| Author | Jesse (`src/sensors/`, branch `jesse/soil-sensor`, based on `d7f10e1` from `jesse/bh1750`) |
| Milestone | M2 (TP-D2) |
| Depends on | PR #3, which depends on #2 and #1. All are unmerged drafts. |
| Fresh-context agent review | Done 2026-10-21. No blocking issues; 2 substantive (documentation) and 6 nits, all resolved (below). |
| **Michael's human review** | **PENDING. Required before merging** (CLAUDE.md rule, clarified in this change). |
| Commit / push | Committed on `jesse/soil-sensor` and pushed as a **draft PR** for Michael to review on GitHub. It replaces the earlier local-patch review procedure. **Not merged.** |
| Hardware | **None used.** No soil sensor, Raspberry Pi or I2C bus was touched. |

Scope is initialisation and raw capacitance acquisition only. Calibration, moisture percentage, trends, fusion and plant-care advice are reserved for the team's own implementation and are not here.

## Sources (downloaded as text, pinned, not committed)

| Source | Version |
|---|---|
| `adafruit/seesaw` firmware | commit `20e2892e4c0d5bcc71b2b0b5bc981c091cdbd10b` |
| `adafruit/Adafruit_Seesaw` (Arduino) | 1.7.9, commit `5b3c3db0c3cc50724c65589fe14c0da3bcf7e9b5` |
| `adafruit/Adafruit_CircuitPython_seesaw` | commit `13ba8ba2a2cecb9df5a41390f991ce6c0cd76e14` |
| Adafruit STEMMA Soil Sensor guide | overview and Pinouts pages, read 2026-10-21 |
| `adafruit/Adafruit_BusIO` (used by the Arduino library) | 1.17.4, commit `3b8364267c3ee6e16bad91bc2101aefbd5b5915f` |
| CircuitPython PR #24; commits `e604940`, `5f9a2b1`; firmware commit `3a39cd0` | read through the GitHub API |

Full SHA-256 of each file read:

```text
b5d4f16c18a9c7be6155d21c38a99fc0a1a4fafe695cdbb9e61da6b58e26dd9f  seesaw  boards/soil/board.mk
70681b3feed31f96c58fa43c1a4d2c150e71c89a5e77a65151dc8059bbfb46cb  seesaw  boards/soil/board_config.h
a77db5352c59b8cf13a089429436568142062fd96bb07ac4cc4b24c776b4aeb3  seesaw  include/RegisterMap.h
1856d1d576153aa4cf0391c188df0ccc880add747da5765f27667e63320f15fe  seesaw  source/Delegate.cpp
3dc4d7193f68fea3c0615f6e90de40dcbd7913d6e93fce59aa178aa43e0cdaf8  seesaw  source/I2CSlave.cpp
e1acd897f1736955ef05a8625a5b4295872f31f95891a1267f5057917b7b8d1f  seesaw  source/AOTouch.cpp
10fea37ec3520005952d8e7370aadce51ca4327b31d41571981e34c7aa660e5f  seesaw  bsp/adafruit_ptc.cpp
9f2298667a75c24fd2e0aacf4f99d874cf1907f0c0a3a9b80507e1d76004ca76  Arduino Adafruit_seesaw.h
98eef4e61c96da8b87c1bc2d1be053d565a5745e1013468b7262d3316ea7e91a  Arduino Adafruit_seesaw.cpp
6b121710b22cfda7693227ff384da7f5b6ae3c662f713b67f177209ad787fbdd  CircuitPython adafruit_seesaw/seesaw.py
88b843d7a44e3f974950d883649e5682f21d9f8ce135daff3a42d015f7cd27e7  BusIO Adafruit_I2CDevice.h
317c342821f1df676a0e034f65685b4d6bf240d2ab8dcea91d18f1e063326d37  BusIO Adafruit_I2CDevice.cpp
```

## Resolved facts

### 1. Board, chip and hardware ID

- **The chip is SAMD10.**
  - Firmware `boards/soil/board.mk`: "CHIP_FAMILY = SAMD10", "CHIP_VARIANT = SAMD10D14AM", "BOARD_NAME = SOIL".
  - The guide agrees: "built into the ATSAMD10 chip".
- **The hardware ID 0x55 is not chip-specific.**
  - Firmware `include/RegisterMap.h` defines `#define SEESAW_STATUS_HW_ID_CODE 0x55` once, with no chip condition.
  - `source/Delegate.cpp` returns it for every build: `case SEESAW_STATUS_HW_ID:{ uint8_t r = SEESAW_STATUS_HW_ID_CODE;`.
  - So 0x55 means SAMD-family seesaw firmware, not a particular chip. We do not infer the chip from it.
- **Where "SAMD09" came from.** The earlier note was the Arduino library's *name* for the value: `#define SEESAW_HW_ID_CODE_SAMD09 0x55`. The board is a SAMD10.
- **Other board facts:** `boards/soil/board_config.h` has "PRODUCT_CODE 4026", "CONFIG_I2C_SLAVE_ADDR 0x36" and "CONFIG_TOUCH0 1".
- **Address range.** Firmware `source/I2CSlave.cpp` starts from `uint8_t addr = CONFIG_I2C_SLAVE_ADDR;` (the soil board has `CONFIG_NO_EEPROM`). It reads ADDR pins `PIN_ADDR_0 16` and `PIN_ADDR_1 17` as bits 0 and 1 (`addrmask = 0x3`) and adds them. The guide says the same: "The default address is 0x36", "between 0x36 and 0x39 inclusive", AD0 "+1", AD1 "+2".

### 2. What values above 4095 mean

- **Origin of the cutoff.** CircuitPython `moisture_read` has `# retry if reading was bad` followed by `while ret > 4095:`, and raises "Could not get a valid moisture reading." after 3 retries. Commit `e604940`, "add retry for moisture read", added it through **PR #24, "Add retries for moisture reading"**.
  - That PR was tested on a **Raspberry Pi with the soil sensor at 0x36**.
  - Before the fix, a loop that stopped on `moisture_read() > 4095` ended after 223, 15 and 283 reads.
  - After the fix it "ran for ~15 minutes".
  - Normal values in the PR were 359, 1015 and 362.
- **Conclusion:** a value above 4095 is an *empirically observed bad read* on the Pi, consistent with corrupted bus data. It is **not** a documented range and **not** a firmware sentinel.
- **The firmware itself.** Its `touch_measure` returns the PTC `RESULT` divided by the oversample count. The soil board uses `TOUCH_OVERSAMPLE OVERSAMPLE_64` (`case OVERSAMPLE_64: return m/64;`), so at this commit the firmware cannot produce more than 65535 / 64 = 1023.
  - Its `return -1` paths are effectively unreachable: `if (m == -1)` compares a `uint16_t` with an `int`, and the final `return -1` follows a switch that covers every case.
- **The Arduino library agrees with the firmware.** `touchRead` is documented as returning "an integer between 0 and 1023".
- **History.** Firmware commit `3a39cd0` (2019-12-15, "add configurable touch settings") changed the soil board to `OVERSAMPLE_64` / `RESISTOR_100K` / `FREQ_MODE_SPREAD`. The code it replaced hard-coded `OVERSAMPLE_4`, giving an arithmetic bound of 65535 / 4 = 16383.
  - PR #24 (2019-02) therefore ran on that **older** firmware. Its normal values (359–1015) were still at or below 1023.
- **The guide disagrees.** It says "about 200 (very dry) to 2000 (very wet)", which is impossible on current firmware (≤ 1023) and possible only on the pre-`3a39cd0` configuration. That the guide predates `3a39cd0` is a hypothesis, not a verified fact. The conflict is **unresolved**, because the shipped firmware version is unknown.
- **Driver choice.** The driver rejects only values above 4095, Adafruit's own threshold, as approved.
  - On firmware `3a39cd0` or later, the cutoff cannot reject a legitimate reading, because those never exceed 1023.
  - On older firmware, that claim rests only on PR #24's empirical values.
  - **Limitation:** on current firmware, values 1024–4095 are impossible but are still accepted, so a corrupted read in that band is not detected.
  - Tightening the cutoff to 1023 is a **team decision** once the real board's firmware VERSION and date code are known. The header documents the same reasoning.

### 1023 versus 4095: firmware behaviour and host rule

These are two different limits from two different places:

| Limit | Where it comes from | What it is |
|---|---|---|
| **1023** | **Sensor firmware** (`bsp/adafruit_ptc.cpp` with `OVERSAMPLE_64`, since `3a39cd0`) | The largest value the chip can *produce*: the 16-bit PTC result divided by 64. It is arithmetic, not a check, and the firmware never reports an error for it. Adafruit's Arduino `touchRead` documents the same range: "an integer between 0 and 1023". |
| **4095** | **Host library** (CircuitPython `moisture_read`, PR #24) | A *rejection rule* on the Pi side. A value above it is treated as a bad read and retried. It was found empirically on older firmware, and it is not a sensor specification. |

Our driver applies the **host rule** (reject above 4095), as approved. It does not apply the firmware bound.

**What accepting 1024–4095 means:**
- **On current firmware**, no genuine reading can fall in 1024–4095, so a value there can only be bad data, such as a corrupted transfer. The driver will **accept and pass it on as if valid**.
  - The driver detects corruption only above 4095. The band 1024–4095 is *impossible but undetected*.
  - Bad data at or below 1023 can't be told apart from a real reading by any range check.
- **On pre-`3a39cd0` firmware** (`OVERSAMPLE_4`, bound 16383), values above 1023 were possible, so rejecting them could discard real readings.
- **We don't know which firmware the board runs**, because VERSION is not read. That is why the approved cutoff stays at 4095 rather than 1023.
- **Consequence for analysis:** a reading in 1024–4095 should be treated as suspect on current firmware. Deciding how to handle it, or tightening the driver to 1023 once the board's firmware version is known, is reserved for the team.

## Protocol used

| Step | Bytes / value | Source |
|---|---|---|
| Software reset | write `{0x00, 0x7F, 0xFF}` | `SWReset`: `write8(SEESAW_STATUS_BASE, SEESAW_STATUS_SWRST, 0xFF)`. The firmware ignores the value (`case SEESAW_STATUS_SWRST:` publishes `SYSTEM_STOP_REQ`), but **one data byte is required**: `I2CSlave.cpp` passes `bytes_received - 2` as the length, and `Delegate.cpp` treats `if(!len)` as a read. |
| Wait after reset | 500 000 µs | Python `sw_reset(self, post_reset_delay=0.5)`. Arduino instead polls `detected()` 10 × 10 ms. |
| Select HW_ID | write `{0x00, 0x01}` | `read(SEESAW_STATUS_BASE, SEESAW_STATUS_HW_ID, &c, 1)` |
| HW_ID wait | 8 000 µs | Python `def read(self, reg_base, reg, buf, delay=0.008)`. The Arduino default is 250 µs. |
| Read HW_ID | 1 byte, must be 0x55 | `RegisterMap.h` |
| Select touch channel 0 | write `{0x0F, 0x10}` | `SEESAW_TOUCH_BASE 0x0F`, `SEESAW_TOUCH_CHANNEL_0 0x10`; `touchRead(0)` |
| Measure wait | 5 000 µs | Python `moisture_read` `self.read(..., buf, 0.005)`. Arduino uses `3000 + retry * 1000`. |
| Read value | 2 bytes, high byte first | Firmware `{ (uint8_t)(valueRead >> 8), (uint8_t)(valueRead & 0xFF) }`; Python `">H"` |

- **One transaction per step.** Each select, wait and read is separate, matching both libraries.
  - CircuitPython is verified: `read()` does `self.write(reg_base, reg)`, `time.sleep(delay)`, then `with self.i2c_device as i2c: i2c.readinto(buf)`.
  - Arduino is verified through Adafruit_BusIO 1.17.4:
    - `Adafruit_seesaw::read` calls `_i2c_dev->write(prefix, 2)`, then `delayMicroseconds(delay)`, then `_i2c_dev->read(buf + pos, read_now)`, passing no `stop` argument.
    - BusIO declares `bool write(const uint8_t *buffer, size_t len, bool stop = true, ...)` and ends with `_wire->endTransmission(stop)`.
    - It also declares `bool read(uint8_t *buffer, size_t len, bool stop = true)`, which calls `requestFrom(..., stop)`.
    - So the register select ends in a STOP, and the read is a separate transaction that also ends in a STOP. This is what the library *requests*; the Arduino `Wire` implementation that honours it was not reviewed.
- **Waits.** Where the libraries differ, the larger *first-attempt* value is used, as approved.
  - Arduino `touchRead` starts at 3000 µs and grows by 1000 µs per retry, up to 7000 µs.
  - Python `moisture_read` uses 5 ms. Python `touch_read` cannot be used on this board (its SAMD09 pin map has no touch pins).
  - **Real hardware timing is unverified.**
- **Later delay change.** CircuitPython commit `5f9a2b1`, "Increase read delay to fix occasional bad reads", raised the *default* read delay from 5 to 8 ms. `moisture_read` still passes 5 ms explicitly, and that is the value used here.

## API: `include/sensors/seesaw_soil.h`

```c
soil_status soil_init(i2c_device *dev);
soil_status soil_measure_capacitance(i2c_device *dev, soil_reading *out);
const char *soil_stage_name(soil_stage stage);
```

- **Stateless.** Every function borrows an already-open `i2c_device`.
- **Preconditions, documented in the header:**
  - a successful `soil_init` is required before measuring, and again after power loss
  - exclusive use of the address
- **Accepted addresses.** Only 0x36–0x39 are accepted. Any other address, a closed device, or NULL arguments give `STAGE_ARGUMENT` with no I/O.
- **Stages.** Each step has its own: `RESET`, `RESET_WAIT`, `HW_ID_SELECT`, `HW_ID_WAIT`, `HW_ID_READ`, `HW_ID` (wrong ID), `MEASURE_SELECT`, `MEASURE_WAIT`, `MEASURE_READ`, `RANGE` (above 4095).
- **Failures.** The first failure stops the call and returns that step's `i2c_result` unchanged. For `HW_ID` and `RANGE`, `rejected_value` holds the ID or the counts.
- **Output.** `*out` (`capacitance_counts`, `is_simulated`) is written only after a complete read of an in-range value.
- **Left out of the driver:**
  - retries and backoff (the caller's job)
  - the VERSION check
  - timestamps
  - any calibration, including the guide's 200–2000 range

## Tests: `tests/test_seesaw_soil.c` (SIMULATED, scripted mock)

| Test | Covers |
|---|---|
| `test_init_sends_documented_sequence` | the exact bytes, order and waits (500 ms + 8 ms); ID 0x55 accepted |
| `test_init_stops_at_each_failure` | `EIO` at each of the 5 init steps, plus a short transfer at each of the 3 transfers. Each gives the right stage, and no later step runs. |
| `test_init_rejects_other_ids` | **all 256 IDs**: 0x55 is accepted, and each of the other 255 gives `STAGE_HW_ID` with the ID reported. This catches masked or extra-accept comparisons. |
| `test_measure_decodes_big_endian` | `{0x01,0xF4}` → 500; `{0x01,0x02}` → 258 (not 513); a wait of at least 5 ms |
| `test_measure_range_boundary` | **all 65 536 values**, decoded high byte first: 0–4095 accepted; 4096–65535 give `STAGE_RANGE`, with the value reported and the output unchanged |
| `test_measure_stops_at_each_failure` | select `EIO` and short write; wait `EINVAL`; read `ENXIO`, 1 of 2 bytes, 0 bytes. Each gives the right stage, leaves the output unchanged and runs no later step. |
| `test_addresses` | the full init-and-measure sequence at each of 0x36–0x39. Addresses 0x00, 0x23, 0x35, 0x3A, 0x49, 0x5C and 0x7F are rejected before any I/O. |
| `test_arguments_rejected` | NULL dev and out; a zeroed device; a device closed after being open at 0x36 |
| `test_simulation_label_copied` | `is_simulated` is copied from the device, not hard-coded |
| `test_stage_names` | every stage, plus `UNKNOWN` |

## Tests-first evidence

1. With the header and tests only, `make test` stopped with `No rule to make target 'src/sensors/seesaw_soil.c'`.
2. Against a throwaway stub (scratchpad only, never in the repo), the tests compiled and linked, then failed:
   `test_init_sends_documented_sequence: Assertion 'mock.next_step == INIT_STEPS' failed` (exit 134).

## Mutation checks (scratch copies, timeout each; all caught)

| Broken guard | Caught by |
|---|---|
| skip the reset write (compiling version); skip the reset wait; 10 ms reset wait; 250 µs HW_ID wait; reset value 0x00 | `test_init_sends_documented_sequence` |
| 3 ms measure wait; touch channel 1; little-endian decode | `test_measure_decodes_big_endian` |
| accept any ID; ID not reported | `test_init_rejects_other_ids` |
| no range check; `>=` cutoff; a rejection that still writes `*out` | `test_measure_range_boundary` |
| continue after a select or wait failure; wrong wait stage | `test_init_stops_at_each_failure` |
| address range widened to 0x3A | `test_addresses` |
| no `ops` check; no NULL `out` check | `test_arguments_rejected` |
| hard-coded `is_simulated` | `test_simulation_label_copied` |
| extra accepted ID 0xD5; masked comparison `(id & 0x7f)` (both survived the reviewer's run before the fix) | `test_init_rejects_other_ids` (exhaustive) |
| range checked on the high byte only | `test_measure_range_boundary` (exhaustive) |
| report the uninitialised `id` on a read failure (reviewer's run) | **Valgrind only** (exit 9). The asserts cannot see uninitialised data. `make memcheck` is mandatory, so this is caught, but not by `make test` alone. |

The first attempt at "skip the reset write" only failed to compile (an unused variable), which proves nothing about the tests. It was re-run as a version that compiles, and the tests caught it.

## Final verification

Run 2026-10-21 on Ubuntu with GCC 15.2.0, GNU Make 4.4.1 and Valgrind 3.26.0, in this order:

| Command | Exit | Result |
|---|---|---|
| `make clean` | 0 | `rm -rf build` |
| `make` | 0 | builds `build/plant-monitor` (the driver is not linked into the product) |
| `make test` | 0 | all six pass lines, including `SIMULATED seesaw soil driver tests passed (scripted mock; no sensor hardware used)` |
| `make asan` | 0 | `build/asan/`: all suites pass, with no ASan/UBSan reports |
| `make memcheck` | 0 | straight after `asan`, no clean: 5 binaries, each `ERROR SUMMARY: 0 errors from 0 contexts` |
| `make test` | 0 | all pass |

Also:
- `make clean && make -j8 asan` (exit 0), then `make -j8 memcheck` (exit 0, 5 × 0 errors).
- The new sources are ASCII-only.
- The analysis code, transport, mock, Linux backend and BH1750 driver are unchanged.

## Not verified / accepted limitations

- **No hardware.** Unverified on the real board: its hardware ID, its address (expected 0x36), the I2C timing and real readings.
- **Waits.** They come from vendor libraries, not a datasheet (Adafruit publishes none for the seesaw protocol), and are unverified on real hardware.
- **Value range.** Firmware since commit `3a39cd0` caps the value at 1023 (OVERSAMPLE_64), while the guide says about 2000 when wet. The shipped firmware is unknown, because VERSION is not read (by decision).
- **The 4095 cutoff** is Adafruit's empirical bad-read threshold, not a specification. Bad reads at or below 4095, including the band 1024–4095 that is impossible on current firmware, are not detected.
- **No retry in the driver.** The Arduino library retries up to 5 times and CircuitPython up to 3. Here the capture layer must retry with backoff (per `docs/design.md`).
- **Exclusive ownership** is documented, not enforced.
- **16/32-bit portability is reasoning, not testing.**
  - No 32-bit build has been linked or run, and no 16-bit-`int` build exists.
  - The reviewer only compiled `seesaw_soil.c` alone, with `-m32 -ffreestanding -c`. That shows the file compiles for 32-bit; it does not show the program works there.
  - The claims below come from reading the source:
    - the byte decode `(uint16_t)bytes[0] << 8` is well defined whether `int` is 16 or 32 bits
    - the wait constants (`500000u`) convert cleanly to `uint32_t`
    - there is no `long` or `size_t` assumption in the decoded value

## Fresh-context review

The reviewer was a fresh-context agent with read-only instructions. Its only network access was read-only `gh api` calls against adafruit repositories.

It verified every protocol fact against the pinned sources:
- the chip, and that 0x55 is not chip-specific
- the registers and the SWRST write
- the waits and their sources
- big-endian decoding and the address range
- PR #24 and commits `e604940` and `5f9a2b1`

**No mismatches.** Reading the source, the reviewer also found no undefined behaviour for 32-bit or 16-bit `int`. That is source-review reasoning, not a tested result (see the 16/32-bit limitation).

By running code, it confirmed:
- `make test`/`asan`/`memcheck` pass.
- `seesaw_soil.c` builds clean under `-Wconversion -Wsign-conversion -Wshadow -Wvla -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes`.
- The single file also compiles with `-m32 -ffreestanding -c`. That is a compile-only check: nothing was linked or run for 32-bit.
- Of 24 mutants, 22 were killed.

| # | Severity | Finding | Resolution |
|---|---|---|---|
| S1 | substantive (docs) | The cutoff comment left out the firmware's 1023 bound, the Arduino "0 and 1023" doc, and the `3a39cd0` OVERSAMPLE history, and so hid the undetected 1024–4095 band. | **Fixed** in the header and this doc. The cutoff stays 4095, as approved. Tightening it is a team question. |
| S2 | substantive (docs) | The header cited "hashes", but the doc had only 8-hex prefixes (all of which matched), and `I2CSlave.cpp` was missing. | **Fixed.** Full SHA-256 for all 10 files are listed above. |
| N1 | nit | The address range was attributed only to the guide. | **Fixed.** It now cites `I2CSlave.cpp` (`CONFIG_I2C_SLAVE_ADDR` plus 2 ADDR bits). |
| N2 | nit | "The firmware ignores its value" was incomplete: a data byte is required. | **Fixed** in the header and the protocol table. |
| N3 | nit (weak test) | The ID mutants `id != 0x55 && id != 0xd5` and `(id & 0x7f) != 0x55` survived. | **Fixed.** The ID test is exhaustive over all 256 values, and the range test over all 65 536. Both mutants are now killed. |
| N4 | nit | Reporting an uninitialised `id` is caught only by Valgrind. | Recorded under mutation checks. `make memcheck` is mandatory. |
| N5 | nit | "The larger where they differ" was ambiguous for the measure wait. | **Fixed.** It now says "first-attempt value". |
| N6 | nit | The Arduino STOP-after-write detail depends on BusIO, which was not read. | **Resolved.** BusIO 1.17.4 `write`/`read` default to `stop = true` (`endTransmission(stop)`, `requestFrom(..., stop)`), and the seesaw library passes no `stop` argument. |

**No substantive findings remain.**

The reviewer also confirmed:
- design conformance and the transport contract
- the CLAUDE.md style rules and the Makefile wiring
- no scope creep
- the CLAUDE.md diff is limited to the separate approval rule

## Michael's review

- [ ] Vendor-source facts (chip, ID 0x55, register map, 4095 provenance)
- [ ] Waits and their sources
- [ ] API (stages, `rejected_value`, output-unchanged rule, address range)
- [ ] Makefile additions (shared)
- [ ] CLAUDE.md rule: feature branches; Michael approves before merging; agent review does not replace his
- [ ] Approved for merge

Reviewer notes: _(pending)_
