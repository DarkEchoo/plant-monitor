# Plant Monitor

Raspberry Pi device that combines soil-moisture measurements with light
measurements to help determine whether a specific plant is receiving enough
sunlight and water.

This repository is for the CS-370 term-project scaffold. Hardware capture,
storage, supervision, and final decision logic are not complete yet.


## Planned hardware

1. Raspberry Pi 3 Model B+ with a soldered header
2. Adafruit STEMMA Soil Sensor (I2C capacitive moisture sensor)
3. Adafruit BH1750 light sensor
4. One Adafruit 4397 JST-SH-to-female-socket cable
5. One Adafruit 4424 JST-SH-to-JST-PH adapter cable
6. One high-quality 5 V, 2.5 A Micro-USB power supply

The Adafruit 4397 cable connects the Raspberry Pi GPIO header to the BH1750.
The Adafruit 4424 cable connects the BH1750 to the soil sensor, allowing both
sensors to share the Raspberry Pi's I2C bus.
[`docs/hardware-plan.md`](docs/hardware-plan.md) before connecting power.

## Why the sensors cooperate

The project records light exposure and soil moisture for the same plant.
The analysis will compare the plant's measured light exposure with its
sunlight needs and use the soil readings to help determine when it may need
water. Together, these measurements give the user a clearer picture of the
plant's growing conditions.

The initial project claim is deliberately narrow:

> For one plant with known sunlight needs and a calibrated soil sensor,
> identify when it may not be receiving enough sunlight or water.

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
- [ ] Real user and specific plant confirmed
- [ ] Partner names and ownership map completed
- [ ] Components electrically verified
- [ ] M1 memo finalized
- [ ] Each partner's raw M1 `.jsonl` files copied into their own folder

Search for `TODO(team)` before submission. Never claim hardware results, user
interviews, calibration, or measurements that have not occurred.

