# Hardware acceptance: sensor-diag on the Raspberry Pi

| Item | Status |
|---|---|
| Tool | `build/sensor-diag` on `main` (merged through PR #8; run from `origin/main`) |
| Who runs it | Michael, on the project Raspberry Pi 3 Model B+ |
| Status | **Procedure only. Not run yet; there are no results.** |
| Assembly reference | `docs/hardware-plan.md` (wiring path, header pins, calibration plan) |

## What this establishes, and what it does not

The procedure has two parts:
- **Part A, basic communication (steps 1–5): pass/fail.** It checks that both sensors answer at their addresses, initialize and return readings marked valid, and that a missing sensor is reported correctly while the other keeps working.
- **Part B, exploratory response (steps 6–7): observations, not pass/fail.** It records how the readings change under obvious changes in light and soil. An unexpected result there is something to investigate and repeat. On its own it does not prove a driver failure.

Neither part establishes:

| Not established | Why | Where it belongs |
|---|---|---|
| Calibration | No reference instrument. Light uses the datasheet's *typical* factor; the accuracy range is 0.96–1.44, about ±20%. Soil counts are raw and uncalibrated. | `docs/hardware-plan.md` calibration plan (team, analysis side) |
| Timing reliability | A few samples can't show whether the 180 ms BH1750 wait (specified at 3.0 V / 25 °C; the board runs at 3.3 V) or the vendor-library soil waits hold over time | Longer runs, planned separately |
| Firmware behavior | The soil firmware version is not read. The soil value range (≤ 1023 on current firmware, "about 200 (very dry) to 2000 (very wet)" in Adafruit's guide) is unresolved. | A separate VERSION diagnostic |
| Full valid measurement range | No extreme light or moisture conditions are produced here | Later testing |
| Long-term stability | Minutes, not days | The **48-hour unattended test** in `docs/design.md`'s evaluation plan, which runs later with the capture processes, **not with sensor-diag** |

`sensor-diag` output is a **diagnostic format, not the storage record format**.

## Rules for the whole session

- **Power:** shut down Raspberry Pi OS (`sudo poweroff`) and unplug the power supply before connecting or disconnecting any cable. Never hot-plug.
- **Recording:**
  - Record every run, including failures and repeats.
  - Don't rerun a step until it passes and keep only the good run. A repeat gets a new step name, for example `light-room-2`.
  - Don't edit the saved output files. Put observations in `notes.txt`.
- **Stopping:** if a Part A step fails, stop, record it, and power off before touching wires.
- **Tools:** don't install software or change system settings (for example enabling I2C) without noting it in `notes.txt`.
- **Privacy:** collect only what is needed. The commands below avoid the hostname and user name. Evidence stays private until it has been inspected (step 8).

## 0. Evidence folder

Run in **bash** (`PIPESTATUS` is a bash feature) on the Pi, from the repository root:

```sh
EVID="$HOME/plant-monitor-evidence/run-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$EVID"
: > "$EVID/notes.txt"

# step NAME CMD...: run CMD, showing stdout live while saving it, and save
# the exact command, stderr and CMD's own exit status. In a pipeline $? is
# tee's status, not CMD's, so the status is taken from PIPESTATUS[0].
step() {
    name=$1; shift
    printf '%s\n' "$*" > "$EVID/$name.cmd"
    "$@" 2> "$EVID/$name.err" | tee "$EVID/$name.out"
    status=${PIPESTATUS[0]}
    echo "$status" > "$EVID/$name.exit"
    cat "$EVID/$name.err" >&2
    echo "[$name] exit $status"
}
```

`sensor-diag` line-buffers its stdout, so its lines appear on screen as they are printed even through `tee`.

The Pi has no real-time clock. The folder name and `sensor-diag`'s `wall_ns` may be wrong if the clock wasn't set (`sensor-diag` prints `clock_sync=unknown` for that reason). Record the actual date and time in `notes.txt`.

## Part A: basic communication (pass/fail)

### 1. Wiring check (powered off)

With the Pi shut down and unplugged:

1. Follow `docs/hardware-plan.md`, which gives the path Pi → 4397 → BH1750 → 4424 → soil sensor, and its header pin table.
2. Trace every wire to its **labelled** pad or pin, not by colour:
   - **power** to physical pin 1 (3V3);
   - **ground** to pin 6;
   - **SDA** to pin 3;
   - **SCL** to pin 5.
3. **3.3 V only. Never connect sensor power to pin 2 or 4 (5 V).** Adafruit's soil-sensor pinout says SDA and SCL each have "a 10K pullup to VIN", so 5 V on VIN would pull the Pi's I2C pins to 5 V.
4. Check that each connector is fully seated and keyed, and that the cables have strain relief.
5. **Address jumpers:** record how they are set.
   - BH1750 ADDR: low gives 0x23, high gives 0x5c.
   - Soil AD0/AD1: none shorted gives 0x36; AD0 adds 1 and AD1 adds 2, for a range of 0x36–0x39.
6. Photograph the wiring, and write in `notes.txt` what you checked.

### 2. System and build identification

Power on, then:

```sh
step model    sh -c "tr -d '\0' < /proc/device-tree/model; echo"
step kernel   uname -srvm
step arch     sh -c 'dpkg --print-architecture; getconf LONG_BIT'
step clock    sh -c 'date -u; uptime'
step tools    sh -c 'cc --version | head -1; make --version | head -1'

git fetch origin
git checkout --detach origin/main
step commit   git rev-parse HEAD
step status   git status --porcelain --untracked-files=no

step clean    make clean
step build    make
step test     make test
```

- **Commit:** `commit` is the `main` commit you tested. Whatever it is, the recorded hash identifies what was tested.
- **Tracked files:** `status` should print nothing.
- **Kernel:** `uname -srvm` deliberately leaves out the hostname.
- **Tests:** `make test` is hardware-free, using mocks only. On the Pi it is also the first build and test on the target architecture; `arch` shows whether it is 32- or 64-bit.
- **Optional:** if Valgrind is installed, also run `step asan make asan` and `step memcheck make memcheck`. If it isn't, note that.

**Pass:** `build` and `test` exit 0.

### 3. Bus detection

```sh
step devnodes ls -l /dev/i2c-*
step groups   id -nG
step buses    i2cdetect -l
```

- **I2C not enabled:** if there are no `/dev/i2c-*` nodes, stop and note it. Enabling I2C changes the system configuration, so record it if you do.
- **Missing tool:** if `i2cdetect` is missing, the package is `i2c-tools`. Record installing it.
- **Permissions:** `groups` shows whether you are in the `i2c` group. Otherwise the commands below need `sudo`; note which you used.
- **Bus number:** choose the bus on the 40-pin header from `i2cdetect -l` (normally 1).

Then set the bus and the two addresses expected from the jumpers in step 1:

```sh
BUS=1            # the header bus identified above
LIGHT_ADDR=0x23  # 0x23 or 0x5c, from the BH1750 ADDR setting
SOIL_ADDR=0x36   # 0x36 to 0x39, from the soil AD0/AD1 setting
printf 'BUS=%s LIGHT_ADDR=%s SOIL_ADDR=%s\n' \
    "$BUS" "$LIGHT_ADDR" "$SOIL_ADDR" >> "$EVID/notes.txt"
step scan i2cdetect -y "$BUS"
```

`i2cdetect` shows addresses without the `0x` (for example `23` or `5c`, and `36`).

**Pass:** both configured addresses appear.
- If either is missing, or a different address appears, stop, power off, and recheck the wiring and jumpers.
- Change the variables only to match the jumpers, never just to fit what the scan showed, and note any change.

`i2cdetect` probes every address. That is acceptable on this bus, which has only our two sensors.

### 4. Basic readings

```sh
DIAG=(./build/sensor-diag --bus "$BUS" \
      --light-addr "$LIGHT_ADDR" --soil-addr "$SOIL_ADDR")

step help    ./build/sensor-diag --help
step usage   ./build/sensor-diag --samples 3   # no --bus: expect exit 2
step basic   "${DIAG[@]}" --samples 3
```

- **`help`:** exit 0. Its text has no `LIVE`/`SIMULATED` label; it isn't a run.
- **`usage`:** exit 2. Its message goes to `usage.err`, unlabelled, and nothing is opened.

**Pass for `basic`:**
- **Exit:** `basic.exit` is `0`.
- **Line count:** with `--samples K`, a successful run prints 2K + 8 lines on stdout, so 14 for K = 3:
  - one start line;
  - for each sensor, one each of `event=open`, `event=init` and `event=close`, plus K `event=measure` lines;
  - one summary line.
- **Labels:** every one of those stdout lines starts with `LIVE ` (the real program has no simulated mode). `grep -vc '^LIVE ' "$EVID/basic.out"` prints `0`. Any `SIMULATED` line would mean the wrong program ran.
- **Stderr:** `basic.err` is empty.
- **Readings:** all six `event=measure` lines show `valid=1`, `stage=NONE` and `i2c=OK`.
- **Summary:** the last line reads `event=summary result=PASS failures=0 valid_readings=6/6`.

**If `basic` exits 1:** keep the output and note which line failed (`stage`, `i2c`, `errno`). Power off and check the wiring before any repeat.

### 5. Disconnect and reconnect (powered off only)

The soil sensor is chained through the BH1750, so only the **soil** sensor is disconnected here: disconnecting the BH1750 would remove both.

1. `sudo poweroff`, wait for shutdown, then unplug the power supply.
2. Unplug cable 4424's JST-PH plug from the soil sensor. Leave the BH1750 connected.
3. Power on, set `BUS`, `LIGHT_ADDR`, `SOIL_ADDR` and `DIAG` again exactly as before (a new shell has lost them), then:

   ```sh
   step disc-scan i2cdetect -y "$BUS"
   step disc-diag "${DIAG[@]}" --samples 3
   ```

   **Pass:**
   - `disc-scan` shows `LIGHT_ADDR` but not `SOIL_ADDR`.
   - In `disc-diag`:
     - all three light readings are `valid=1`;
     - soil reports a failure line with its `stage`, `i2c` and `errno`, and **no soil value** (no soil `event=measure` line with `valid=1`);
     - the summary is `result=FAIL`, and `disc-diag.exit` is `1`.
   - The soil open may still succeed, because opening `/dev/i2c-N` doesn't talk to the sensor; the failure would then show at `event=init`. Record the exact errno; don't assume it.
4. `sudo poweroff`, unplug the power, reconnect the JST-PH plug, power on, set the variables again, then:

   ```sh
   step reconn-scan i2cdetect -y "$BUS"
   step reconn-diag "${DIAG[@]}" --samples 3
   ```

   **Pass:** both configured addresses appear, and `reconn-diag` meets every step 4 pass condition.

## Part B: exploratory response (observations, not pass/fail)

Both sensors are read in every run. For each condition, record:
- what was set up, in `notes.txt` before the run;
- whether every reading was `valid=1`;
- the values actually printed.

**If a reading is not valid, stop.** That is a Part A communication failure, not a response observation.

**If the readings respond unexpectedly:**
1. Don't conclude a driver failure.
2. Check the setup: position, cover, light source, other lights, probe contact.
3. Repeat under a new step name, and record both runs.

### 6. Light response (BH1750)

**Setup:**
- Fix the sensor's position *and* the direction it faces before the first run, for example by taping it down and marking it. Don't move it between conditions.
- Keep bright sources moderate. At this driver's settings the largest reading is 65535 counts, `value=54612500` mlx (about 54 600 lx). Bright sources such as direct sunlight can exceed that.
- If a light line shows `raw_count=65535`, treat it as **saturated**: the true level is unknown. Reduce the light or increase the distance, and repeat.

```sh
step light-covered "${DIAG[@]}" --samples 5
step light-room    "${DIAG[@]}" --samples 5
step light-bright  "${DIAG[@]}" --samples 5
```

1. **Covered:** an opaque cover over the sensor, not pressing on the board.
2. **Room:** normal room light.
3. **Bright:** a phone torch at a noted, fixed distance and angle.

**Record:**
- **Values:** the light `value` and `raw_count` of each sample.
- **Ordering:** a covered < room < bright ordering is the expected response. Anything else is an observation to investigate and repeat (setup checks above). On its own it doesn't prove a failure.

**How to read it:**
- **Zero is a real reading.** In darkness the BH1750 can read 0–3 counts (0–2500 mlx). `valid=1 value=0` is a real low reading, while a failure prints `valid=0 value=none`.
- **Suspicious results:** watch for identical values across very different conditions, or zero in bright light. Note and repeat them. These match the stale-or-zero symptom the BH1750 review asked hardware testing to look for.
- **Not lux:** the values are not calibrated lux. Compare them with each other only.

### 7. Soil probe handling and response

**Handling.** Adafruit's guide (overview and pinouts pages) and product page 4026 give no instructions on insertion depth or water exposure (checked 2026-10-24). These are **our own precautions**, not vendor instructions:
- Hold the sensor by its edges.
- Push only the probe section into the soil. Keep the top of the board, the JST-PH connector and the cable out of the soil and dry.
- Never pour water onto the board. Water the soil away from it and dry any splashes.
- Don't touch the probe during a run: a capacitive sensor responds to hands.
- Leave the cable slack, so moving the pot can't pull the connector.

Changing the probe's position is not connecting hardware, so the Pi may stay powered. Write the condition and the probe position (depth, distance from the stem) in `notes.txt` before each run:

```sh
step soil-air   "${DIAG[@]}" --samples 5
step soil-pot   "${DIAG[@]}" --samples 5
```

1. **Air:** the probe touches nothing.
2. **Pot:** the probe is inserted into the plant's soil as it is now.
3. **Optional:** water the soil (away from the board), wait a noted time, then run `step soil-watered "${DIAG[@]}" --samples 5`.

**Record:**
- **Values:** the soil `value` (`unit=raw_counts`) of each sample in each condition.
- **Difference:** the observed difference between air and pot, as it is.
  - There is **no required size of difference**.
  - The difference is **not a calibration**, and the counts are not percentages.
  - The counts say nothing about the firmware version.
- **RANGE rejections:** if a line shows `stage=RANGE` with `rejected_value`, the board returned a value above 4095. Keep it as evidence.

## 8. Hand-over

```sh
tar -czf "$EVID.tar.gz" -C "$(dirname "$EVID")" "$(basename "$EVID")"
```

**Share it privately first.** Give the archive and photos to Jesse directly, not in a public place. The repository and its PRs are public.

**Before anything is published:**
1. **Both partners inspect the evidence for personal information:**
   - user names, home paths or hostnames that appear in output despite the precautions;
   - network details;
   - photo metadata, especially GPS location in EXIF;
   - anything in the photo backgrounds.
2. **Agree what is published.** Keep the original, unedited files privately.
3. **If something must be removed,** publish a separate copy and record in it exactly what was removed and why. Never alter values.

Write any summary separately from the raw files, and include:
- who ran it and when;
- the commit hash;
- the bus and addresses;
- `sudo` or `i2c` group;
- any deviations from this procedure.

### Results summary (fill in only from the saved files)

| Step | Part | Exit | Observation (from the `.out`/`.err` files and `notes.txt`) |
|---|---|---|---|
| build / test | A | | |
| scan | A | | |
| basic | A | | |
| disc-scan / disc-diag | A | | |
| reconn-scan / reconn-diag | A | | |
| light-covered / room / bright | B | | |
| soil-air / pot | B | | |

Passing Part A shows basic communication and responsiveness only. Part B records observed responses. Neither establishes calibration, timing reliability, firmware behavior or the full measurement range. The 48-hour soak is a separate, later test.
