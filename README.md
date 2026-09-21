# Room Comfort Monitor

A Raspberry Pi device that combines temperature/humidity measurements with
occupancy measurements to identify uncomfortable or moisture-prone room
conditions while someone is actually using the room.

This repository is an early CS 370 term-project scaffold. Hardware support and
the final analysis logic are intentionally not implemented yet.

## Proposed hardware

- Raspberry Pi with a 40-pin header
- DHT22 temperature/humidity sensor
- HC-SR501 PIR occupancy sensor

The DHT22 counts as one physical sensor even though it reports two values. The
PIR is the required second physical sensor. The planned decision pipeline will
combine rolling temperature/humidity trends with occupancy state instead of
treating the sensors as two unrelated demonstrations.

## Build and test

```bash
make
make test
make asan
make memcheck
```

Run the development simulator:

```bash
./build/comfort-monitor --simulate
```

All simulated output is labeled `SIMULATED`; the graded soak test and live demo
must use live sensors.

## Repository map

- `src/` - systems core in C17
- `include/` - public C headers
- `tests/` - automated tests
- `docs/problem-memo.md` - M1 problem memo draft
- `docs/design.md` - M2 design-document outline
- `transcripts/` - raw per-partner Claude Code `.jsonl` copies
- `promptlogs/` - one `PROMPTLOG.md` per partner
- `scripts/` - deployment and soak-test helpers (to be added)

## Milestone status

- [x] Repository initialized
- [x] `CLAUDE.md` initialized
- [ ] Real user and exact problem confirmed
- [ ] Partner names and ownership map filled in
- [ ] Ordered hardware confirmed in the documentation
- [ ] M1 memo finalized
- [ ] Each partner's raw `.jsonl` files copied into their own folder

## Before the first push

Search for every `TODO(team)` marker and replace it with accurate team details.
Do not claim hardware, measurements, or user interviews that have not occurred.

