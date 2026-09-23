# Plant Monitor

Raspberry Pi device that combines soil-moisture content
measurements along with light measurements to learn how a specific plant's
soil dries under different light exposure and provides evidence-based watering
advice.

This repository is for the CS-370 term-project scaffold. Hardware capture,
storage, supervision, and final decision logic are not complete yet.

## Planned hardware

1. Raspberry Pi 3 Model B+ with a soldered header
2. Adafruit STEMMA Soil Sensor (I2C capacitive moisture sensor)
3. Adafruit BH1750 light sensor
4. One STEMMA QT-to-GPIO female-socket cable
5. One STEMMA QT-to-STEMMA adapter cable

The Pi cable connects to the GPIO header, and the
two sensors share the I2C bus through their STEMMA QT connectors. See
[`docs/hardware-plan.md`](docs/hardware-plan.md) before connecting power.

## Why the sensors cooperate

The project does not treat moisture and light as unrelated displays. It records
daily light exposure, soil moisture, and the rate of moisture loss. The analysis
will compare drying behavior under different light conditions and
verify that watering caused a plausible moisture increase.

The initial project claim is deliberately narrow:

> For one calibrated plant and soil setup, identify if the plant is receiving enough
> sunlight and water.

## Build and test

```bash
make
make test
make asan
make memcheck
```

Run the development simulator:

```bash
./build/plant-monitor --simulate
```

All generated input and output is labeled `SIMULATED`. The graded soak and live
demonstration must use live sensors.

## Repository map

- `src/` - C17 systems core and early analysis scaffold
- `include/` - public C headers
- `tests/` - automated tests
- `docs/hardware-plan.md` - wiring plan
- `docs/problem-memo.md` - M1 problem-memo draft
- `docs/design.md` - M2 design-document outline
- `transcripts/` - raw per-partner Claude Code `.jsonl` copies
- `promptlogs/` - one `PROMPTLOG.md` per partner

## Milestone status

- [x] Repository and `CLAUDE.md` initialized
- [x] Raspberry Pi 3 Model B+ selected
- [ ] Exact Raspberry Pi choice recorded
- [ ] Real user and specific plant confirmed
- [ ] Partner names and ownership map completed
- [ ] Components electrically verified
- [ ] M1 memo finalized
- [ ] Each partner's raw M1 `.jsonl` files copied into their own folder

Search for `TODO(team)` before submission. Never claim hardware results, user
interviews, calibration, or measurements that have not occurred.

