# Design document outline (M2)

This is an outline, not a completed design document.

## Architecture

Planned processes:

* Soil-capture process: collects soil-moisture readings through I2C
* Light-capture process: collects light readings in lux through I2C
* Analysis/storage hub: calculates daily light exposure, tracks moisture changes, and stores the results
* Supervisor: checks process heartbeats and restarts a process if it stops responding
* Optional LAN-only interface: displays the plant’s current conditions and recent history

The main goal is to determine whether the plant’s current location provides enough light. Soil moisture will provide additional context so the device can distinguish between low-light conditions and possible watering problems.

Proposed sampling rates (to be confirmed by both partners):

* BH1750: one reading every 10 seconds
* Soil sensor: one reading every 60 seconds

Main data flow:

```text
BH1750 (0x23) --I2C via /dev/i2c-1, 1 reading / 10 s--> light-capture
light-capture --Unix-domain socket, 1 message / 10 s (reading + both timestamps + driver status)--> hub

Soil sensor (0x36) --I2C via /dev/i2c-1, 1 reading / 60 s--> soil-capture
soil-capture --Unix-domain socket, 1 message / 60 s (reading + both timestamps + driver status)--> hub

hub --analysis on each new reading--> current plant-care recommendation
hub --batched append, about 7 readings / min--> log file on microSD (fdatasync per batch)

supervisor --starts and restarts (fork/exec, waitpid)--> light-capture, soil-capture, hub
light-capture, soil-capture, hub --pipe per child, 1 heartbeat / 10 s--> supervisor
```

Storage and analysis both run inside the hub process. Each process is planned to be single-threaded: each capture process runs one read-and-send loop that wakes every 10 seconds (the light process reads its sensor on every wake, and the soil process reads its sensor on every sixth wake), and the hub waits on both sockets in one event loop.

Heartbeats (proposed, to be confirmed by both partners):

* Each child sends one heartbeat every 10 seconds, starting right after it starts up, which is well within the 60-second startup requirement.
* The supervisor creates one pipe per child before starting it. It watches all the heartbeat pipes and child exits using `poll` and `waitpid`.
* A process is treated as hung after 30 seconds without a heartbeat.
* A sensor error does not cause a restart. The capture process keeps sending heartbeats marked `DEGRADED` and retries the sensor itself.
* Heartbeats keep running during retry backoff. The backoff is a timed wait inside the same 10-second wake loop, never one long blocking sleep, so a failing sensor never makes the process look hung, however long the sensor stays down. When to re-run `bh1750_init` or `soil_init` after failures will be agreed during the capture-process design.
* A process that crashes or goes silent is restarted.

TODO: Decide how the optional LAN interface reads from the hub.

## Mechanism mapping

* **D — Custom storage layer:** The system will use an append-only log with batched writes and an `fsync` policy. We will test whether the log can recover after a write is interrupted.
  * *Why:* The analysis needs days of history, not single readings: daily light exposure, repeated low-light days, and soil-moisture trends. The Raspberry Pi has no battery, so power can be lost in the middle of a write. With an append-only log, a power loss during a write can only damage the end of the file, so recovery can drop an incomplete final record and keep every earlier one that was synced. Batching writes limits wear on the microSD card, and writing the layer ourselves lets us choose and defend when data is flushed with `fsync`.
* **E — Multi-process architecture:** The sensor processes will send readings to a central hub through Unix-domain sockets. A supervisor will detect and restart a process if it crashes.
  * *Why:* The two sensors fail independently, and the failure-mode table requires a failed sensor to be marked unavailable while the healthy one keeps reporting. Running each sensor in its own process means a crash or stuck read in one capture process cannot stop the other or corrupt the hub's log. The supervisor turns a crash into a recorded exit status and a restart with backoff, giving the evidence the failure-mode table lists instead of a silent stop.

## Failure-mode table

| Component       | Failure                               | Detection                              | Response                                                         | Required evidence                               |
| --------------- | ------------------------------------- | -------------------------------------- | ---------------------------------------------------------------- | ----------------------------------------------- |
| Soil capture    | Sensor disconnected, I2C read fails, or reading rejected | Failed transaction or driver rejection (`HW_ID`, `RANGE`) | Mark moisture as unavailable and retry with backoff | Error count, failure stage, and DEGRADED-state log |
| Light capture   | Sensor disconnected or I2C read fails | Failed driver step                     | Mark light as unavailable and stop making light recommendations  | Error count, failure stage, and DEGRADED-state log |
| Capture process | Process is killed or crashes          | Child exit status or missed heartbeat  | Restart the failed process without stopping the healthy process  | Exit status, backoff, and restart log           |
| Storage         | Write is interrupted or incomplete    | Return value and recovery scan         | Ignore the incomplete final record and preserve valid records    | Recovery count and affected byte offset         |
| I2C bus         | Both sensors stop responding          | Errors from both capture processes     | Report a shared bus failure and retry without inventing readings | Errors from both sensors and degraded-state log |

## Storage and data

Proposed storage plan (to be confirmed by both partners).

### Record format

The hub stores one fixed 48-byte binary record for each sensor reading. Light and soil readings are stored as separate records because they are sampled at different rates (every 10 seconds and every 60 seconds), so a combined row would have to pair readings that were not taken together. Each record is written field by field in little-endian order so the layout does not depend on compiler padding.

| Bytes | Field | Contents |
| ----- | ----- | -------- |
| 0-3 | Magic | The ASCII bytes `PLNT` (`0x50 0x4C 0x4E 0x54`) in this file order; read as a little-endian u32 this is `0x544E4C50` |
| 4 | Version | Record-format version, currently 1 |
| 5 | Record type | 1 = light, 2 = soil, 3 = START (written each time the hub starts) |
| 6-7 | Flags | Bit 0 `VALID`, bit 1 `SIMULATED`, bit 2 `CLOCK_UNTRUSTED`, bit 3 `TIME_UNAVAILABLE`; other bits are 0 |
| 8-15 | Sequence number | Assigned by the hub, increases by one for every record |
| 16-23 | Wall-clock timestamp | `CLOCK_REALTIME` in nanoseconds, read by the capture process when the measurement attempt completes |
| 24-31 | Monotonic timestamp | `CLOCK_MONOTONIC` in nanoseconds, read by the capture process right after the wall clock |
| 32-35 | Sensor value | Valid light: `illuminance_mlx` (millilux, lux × 1000) from `bh1750_raw_to_mlx`. Valid soil: raw `capacitance_counts`. Invalid soil: the driver's `rejected_value`. Otherwise 0 |
| 36 | I2C status | `i2c_status` (u8) from the failing step's `i2c_result` |
| 37 | Failure stage | Driver stage (u8): `bh1750_stage` for light, `soil_stage` for soil |
| 38-39 | errno | `os_errno` (u16) from the failing step's `i2c_result` |
| 40-43 | Source sequence number | Counter from the capture process, used to detect lost or duplicated readings |
| 44-47 | CRC-32 | CRC-32/IEEE of bytes 0-43, stored little-endian |

**Valid reading.** `VALID` is set only when the driver reports overall success (`stage == BH1750_STAGE_NONE` or `SOIL_STAGE_NONE`). I2C status alone is not enough: a soil `SOIL_STAGE_HW_ID` or `SOIL_STAGE_RANGE` rejection has `io.status == I2C_STATUS_OK`, and its record is still invalid.

**Failure detail.** An invalid record keeps the driver's failure stage and, when there is one, the rejected value, so it can say why it is invalid. A valid record has bytes 36-39 all 0, and a START record has bytes 32-39 all 0. Three rules follow from the driver result types:

* The two stage enums reuse the same numbers for different steps (for example, 2 is `BH1750_STAGE_POWER_ON` for light and `SOIL_STAGE_RESET` for soil), so byte 37 is decoded using the record type in byte 5.
* `os_errno` is an `int`. Linux errno values are small positive numbers, so they fit in u16. A value outside 0-65535 is stored as `0xFFFF`.
* Only the soil driver has a rejected value: the ID read for `SOIL_STAGE_HW_ID` and the counts for `SOIL_STAGE_RANGE`. `bh1750_status` has none, so an invalid light record stores 0 in bytes 32-35.

The record does not store `requested_len` or `transferred_len` from `i2c_result`. For a short transfer, those go in the text log.

**Timestamps.** Both timestamps are taken in the capture process, never by the hub. Right after each measurement attempt completes, successful or not, the capture process reads `CLOCK_REALTIME` and then `CLOCK_MONOTONIC`. The timestamps therefore mark when the sensor read completed, not when it started, which matches `sensor-diag`. Both values travel to the hub in the socket message with the reading and driver status. If either `clock_gettime` call fails, that timestamp is stored as 0, `TIME_UNAVAILABLE` is set, and the `errno` is logged. No time is ever invented.

**Clock trust.** At the same moment, the capture process checks whether the wall clock is trusted and sets `CLOCK_UNTRUSTED` if it is not (see "Clock trust" under "Files and retention").

**CRC.** The checksum is CRC-32/IEEE with the reflected polynomial `0xEDB88320`, initial value `0xFFFFFFFF`, and final XOR `0xFFFFFFFF`. Its check value for the ASCII string `123456789` is `0xCBF43926`.

Only raw readings are stored. Daily light exposure, recent soil-moisture change, and the plant-care recommendation are recalculated from these records, so they cannot disagree with the data they came from. Records always use the sequence number for ordering, not the wall clock, because the Raspberry Pi 3B+ has no real-time clock and its time can be wrong after a power loss with networking disconnected. See "Clock trust" below for when the wall clock is used at all.

### Files and retention

**Clock trust.** The wall clock is trusted only while Linux reports that it has synchronized during the current boot. The check is `adjtimex()` called with `modes = 0`: the clock is trusted when the call returns a value other than `TIME_ERROR` and `STA_UNSYNC` is clear. The kernel starts every boot with `STA_UNSYNC` set, and only a time-synchronization daemon clears it. With networking disconnected, the clock therefore stays untrusted, which is the normal case for this device. The state is checked fresh every time it is needed, never remembered: the kernel can set `STA_UNSYNC` again if synchronization stops. If `adjtimex()` fails, the clock is treated as untrusted and the `errno` is logged.

**File names and rotation.** Each log file is named after the sequence number of its first record: `plant-<16 hex digits>.log`, or `SIMULATED-plant-<16 hex digits>.log` for simulated runs. The hub starts a new file when the next record would make the current file larger than 1 MiB (1,048,576 bytes). That limit is 21,845 records, about two days of readings. When the clock is trusted, the hub also starts a new file at each UTC date change. While the clock is untrusted, the date is never used.

**32 MiB cap.** Total log storage is limited to 32 MiB (33,554,432 bytes), counting every `plant-` and `SIMULATED-plant-` file in the log directory, whether the clock is trusted or not. At startup and before opening each new file, the hub deletes the oldest files by first sequence number until the existing files plus one full 1 MiB file fit within 32 MiB. It never deletes the file being written.

**30-day deletion.** When the clock is trusted, files older than 30 days are also deleted when the hub starts and at each UTC date change. A file's age comes from the wall-clock timestamp of its last record, and only if that record has `CLOCK_UNTRUSTED` and `TIME_UNAVAILABLE` clear. A file without such a record is never deleted by date, only by the size cap. While the clock is untrusted, nothing is deleted by date.

**Logging.** Every deletion is logged with the file name, its sequence-number range, and the reason (size cap or age).

At about 10,000 records per day, this is roughly 0.46 MiB per day and 14 MiB for 30 days, so the 32 MiB cap keeps about 67 to 69 days of history.

### Batching and fsync

The hub buffers records in memory and writes them at least every 60 seconds, or sooner when the buffer reaches 32 records. After each batch it calls `fdatasync`, so synced data survives a crash or power loss. It also flushes before a clean shutdown and before switching to a new file, and it syncs the log directory after creating or deleting a file.

The device requires continuous external power and has no built-in battery. A sudden power loss can still lose up to about 60 seconds of readings that were not synced yet. The analysis treats that time as unknown instead of pretending readings existed.

### Recovery

When the hub starts, it:

1. Scans the most recent log file, which is the one containing the highest valid sequence number. It never uses the wall clock to choose, because the Raspberry Pi's clock can be wrong after a power loss. It reads the file one 48-byte record at a time and checks the `PLNT` magic value, version, CRC, and sequence order.
2. Keeps every valid record.
3. Removes an incomplete or damaged end of the file with `ftruncate`, then syncs the file. A damaged record in the middle of the file is skipped and counted instead of deleting the valid records after it.
4. Continues the sequence number from the last valid record.
5. Records what recovery did in the logs: bytes removed, the byte offset of the damage, skipped records, and the last valid sequence number.
6. Writes a START record so the analysis treats the time before it as a gap.

## Analysis plan

The BH1750 will measure the amount of light reaching the plant throughout the day. The system will combine these readings over time instead of making a decision from one reading. This will allow it to estimate daily light exposure and identify repeated low-light days.

The soil sensor will track whether the soil is wet, stable, or drying. This information will help the system produce recommendations such as:

* The plant is receiving enough light and its moisture level is normal.
* The plant is not receiving enough light, but its soil moisture is normal.
* The plant is receiving enough light, but its soil is drying quickly.
* The plant has low light and wet soil, so adding more water may not help.
* A sensor is unavailable, so the system cannot make a complete recommendation.

The system will be calibrated for one specific plant and location. Because the BH1750 measures illuminance in lux rather than plant-usable photosynthetic radiation, the project will describe its result as an estimate of light exposure rather than an exact biological measurement.

## Constraints and substitutions

* The breadboard is omitted by using keyed JST-SH and JST-PH cables.
* The Raspberry Pi connects to the BH1750 through an Adafruit 4397 cable.
* The BH1750 connects to the soil sensor through an Adafruit 4424 adapter cable.
* Both sensors share one I2C bus, which simplifies wiring but creates a shared failure point.
* The Raspberry Pi 3 Model B+ was selected because it has a soldered GPIO header and is easier to develop on than a smaller board.
* The light sensor measures lux rather than photosynthetically active radiation.
* The soil reading will be treated as a relative value calibrated for one plant and pot, not as a universal moisture percentage.
* Simulated and replayed readings will always be clearly labeled.

## Evaluation plan

The evaluation will include:

* Sample-to-storage latency under normal and high CPU load
* CPU usage and memory usage for each process
* Missing or duplicated samples using sequence numbers
* Sensor-process restart time after a process is killed
* Recovery after an interrupted log write
* Accuracy of the device’s light classification against manually recorded test conditions
* Consistency of daily light measurements when the sensor remains in the same location
* Differences in measured light exposure after moving the plant to brighter and darker locations
* System behavior when either sensor is disconnected
* A 48-hour unattended test using both live sensors

TODO(team): Set numerical targets and document the method used for each measurement.

## Ownership map

* **Jesse:** `src/sensors/` and I2C capture
* **Michael:** `src/storage/` and `src/analysis/`
* **Shared and reviewed by both:** supervisor, interface, tests, and documentation

Ownership means first authorship and defense responsibility, not exclusivity.

## AI-use plan

Claude Code may help us plan components, understand Linux and I2C APIs, review changes, and develop tests. It will not run on the device or make decisions for the product. The product source will not contain an external network client or connect to an AI service.

We will check Claude Code's work in these ways:

* **Evidence before acceptance:** A change is accepted only after the build, tests, and applicable sanitizer checks pass, with the actual command output shown.
* **Fresh-context review:** Each milestone diff is reviewed by a Claude Code agent that did not write it, and the findings are recorded under `docs/reviews/`.
* **Partner review:** The partner who did not write a change reviews it before it is merged.
* **Prompt logs:** Each partner keeps a `PROMPTLOG.md` in `promptlogs/` with annotated episodes, including revised plans and agent-introduced bugs we caught.
* **Raw transcripts:** At every milestone M1-M5, each partner copies their raw `.jsonl` session files into `transcripts/` without editing them.
