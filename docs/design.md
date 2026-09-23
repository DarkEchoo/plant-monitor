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

TODO: Add the final sampling rates and label every IPC and storage flow.

## Mechanism mapping

* **D — Custom storage layer:** The system will use an append-only log with batched writes and an `fsync` policy. We will test whether the log can recover after a write is interrupted.
* **E — Multi-process architecture:** The sensor processes will send readings to a central hub through Unix-domain sockets. A supervisor will detect and restart a process if it crashes.

## Failure-mode table

| Component       | Failure                               | Detection                              | Response                                                         | Required evidence                               |
| --------------- | ------------------------------------- | -------------------------------------- | ---------------------------------------------------------------- | ----------------------------------------------- |
| Soil capture    | Sensor disconnected or I2C read fails | Failed transaction or missed heartbeat | Mark moisture as unavailable and retry with backoff              | Error count and degraded-state log              |
| Light capture   | Sensor disconnected or I2C read fails | Failed transaction or missed heartbeat | Mark light as unavailable and stop making light recommendations  | Error and restart log                           |
| Capture process | Process is killed or crashes          | Child exit status or missed heartbeat  | Restart the failed process without stopping the healthy process  | Exit status, backoff, and restart log           |
| Storage         | Write is interrupted or incomplete    | Return value and recovery scan         | Ignore the incomplete final record and preserve valid records    | Recovery count and affected byte offset         |
| I2C bus         | Both sensors stop responding          | Errors from both capture processes     | Report a shared bus failure and retry without inventing readings | Errors from both sensors and degraded-state log |

## Storage and data

Each record will contain:

* Wall-clock timestamp
* Monotonic timestamp
* Sequence number
* Raw soil-moisture reading
* Light reading in lux
* Sensor-validity flags
* Process state
* Accumulated daily light exposure
* Recent soil-moisture change
* Current plant-care recommendation

TODO(team): Decide the final sampling rates, record format, retention period, write-batching policy, and power-loss behavior.

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

## AI-use plan

Claude Code may help us plan components, understand Linux and I2C APIs, review changes, and develop tests. It will not run on the device or make decisions for the product. The product source will not contain an external network client or connect to an AI service.
