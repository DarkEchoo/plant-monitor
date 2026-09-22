# Design document outline (M2)

This is an outline, not a completed design document.

## Architecture

Planned processes:

- Soil-capture process: low-rate I2C moisture samples
- Light-capture process: higher-rate I2C illuminance samples
- Analysis/storage hub: rolling features, state, and append-only records
- Supervisor: heartbeats, degraded state, restart with backoff
- Optional LAN-only interface: read-only status and history

TODO(team): Add measured rates and label every IPC and storage flow.

## Mechanism mapping

- D: append-only log, batching, `fsync` policy, and crash-recovery experiment.
- E: isolated capture processes, Unix-domain socket hub, and supervising parent.

## Failure-mode table

| Component | Failure | Detection | Response | Required evidence |
|---|---|---|---|---|
| Soil capture | Sensor absent/I2C read error | Failed transaction or missed heartbeat | Mark moisture unavailable; retry with backoff | Error count and degraded transition |
| Light capture | Sensor absent/I2C read error | Failed transaction or missed heartbeat | Continue moisture capture without watering advice | Error and restart log |
| Capture process | Process killed | Child status or missed heartbeat | Restart without stopping healthy process | Exit status, backoff, restart |
| Storage | Interrupted or short write | Return value and recovery scan | Discard incomplete tail record | Recovery count and byte offset |

## Storage and data

Store monotonic and wall-clock timestamps, sequence number, raw moisture value,
lux, validity flags, process state, and derived rolling features. TODO(team): Set
sample rates, record format, retention, batching, and power-loss behavior.

## Constraints and substitutions

- The breadboard is omitted by using keyed JST-SH and JST-PH cables.
- The Raspberry Pi connects to the BH1750 through an Adafruit 4397 cable.
- The BH1750 connects to the soil sensor through an Adafruit 4424 adapter cable.
- Both sensors share one I2C bus, which simplifies wiring but creates a shared
  failure point.
- The Raspberry Pi 3 Model B+ was selected for its soldered GPIO header and
  easier development experience.

## Evaluation plan

TODO(team): Set numerical targets and methods for sample-to-storage latency,
CPU/RSS, sequence gaps, sensor-process restart time, interrupted-log recovery,
watering-response detection, and the required 48-hour live-sensor soak.

## Ownership map

TODO(team): Replace ownership placeholders in `CLAUDE.md` with both names.

## AI-use plan

Claude Code may help plan, explain Linux/I2C APIs, review diffs, and propose
tests. It may not run in the device or make runtime decisions. The product source
will contain no external network client.

