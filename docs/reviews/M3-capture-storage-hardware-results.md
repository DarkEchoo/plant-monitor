# M3 hardware results: live capture to storage on the Raspberry Pi

| Item | Value |
|---|---|
| Run | Evidence folder `run-20261028T211418Z` (UTC, from the Pi clock) |
| Who | Claude Code over SSH, at Michael's request. Nobody was at the Pi. |
| Commit | `68c70de1ec14cf6e4f6f8642807e1b973a538d66` on `michael/capture-storage` (`commit.out`, `branch.out`); `status.out` empty |
| Hardware | Raspberry Pi 3 Model B Plus Rev 1.4, aarch64, 64-bit (`model.out`, `arch.out`) |
| Bus and addresses | Bus 1; BH1750 `0x23`; soil sensor `0x36` |
| Access | User in the `i2c` group; no `sudo` used |
| Raw evidence | Kept privately as `run-20261028T211418Z.tar.gz` (SHA-256 `36686a8bc856a531575e93f95c30766e59facbff115f283af54e2c4fde068223`). Not in this repository. |

This file is a sanitized summary written only from the saved `.out`, `.err` and `.exit` files and `notes.txt`. User names, home paths, the hostname and network details are left out on purpose.

## Temporary integration: read this first

`plant-capture` is a **temporary per-sensor integration for the M3 demo**, not the design's hub:
- **Separate processes and logs:** there is one process per sensor, and each writes its own storage log in its own directory (`store/light/`, `store/soil/`).
- **Sequence numbers are per process (per file), not hub-wide.** They say nothing about ordering between light and soil records.
- **No recovery, rotation or retention.** Every run uses fresh directories, and a previous run's log is refused, not recovered.

## Results summary

The rows are the runbook's (`docs/hardware-acceptance.md`), plus rows for the M3 capture-to-storage check.

| Step | Part | Exit | Observation (from the saved files and `notes.txt`) |
|---|---|---|---|
| build / test | A | 0 / 0 | `make` built `plant-monitor`, `sensor-diag`, `plant-capture` and `storage-dump`. All 13 test binaries passed. |
| asan / memcheck | A | 0 / 0 | ASan and UBSan passed the same 13 binaries. Valgrind 3.24.0 reported 0 errors in all 13 runs, with all heap blocks freed. |
| scan | A | 0 | `0x23` and `0x36` both present on bus 1, nothing else. Called via `/usr/sbin/i2cdetect` (PATH). |
| basic | A | 0 | 14 lines, all `LIVE`, empty stderr. `result=PASS failures=0 valid_readings=6/6`. Light 10833, 11667 and 10833 mlx; soil 836, 799 and 857 raw counts. |
| disc-scan / disc-diag | A | — | **Not run.** Needs the Pi powered off and the soil cable unplugged by hand. |
| reconn-scan / reconn-diag | A | — | **Not run** (as above). |
| light-covered / room / bright | B | — | **Not run.** Needs controlled light conditions. |
| soil-air / pot | B | — | **Not run.** Needs the probe moved by hand. |
| cap-help / cap-usage | M3 | 0 / 2 | Without `--store-dir`, the program exits 2 with a usage message. |
| cap-light | M3 | 0 | Ran 190 s alongside cap-soil, then stopped by SIGTERM. 19 samples, all valid. 19 heartbeats, first one on line 4 (right after storage opened). `exit result=STOPPED`, device close OK, store close OK. Empty stderr. |
| cap-soil | M3 | 0 | Same run. 4 samples (every 6th tick), all valid. 19 heartbeats, first on line 4. `exit result=STOPPED`, close OK. Empty stderr. |
| cap-fds | M3 | 0 | 30 s into the run, each process had only fds 0–2, `/dev/i2c-1` and its own log file open. |
| cap-procs-left | M3 | 0 | `none`: no `plant-capture` process left after the stop. |
| dump-light | M3 | 0 | `records=20 start=1 light=19 light_valid=19 bad=0 sequence_breaks=0 partial_tail_bytes=0 result=PASS`. File is 960 B = 20 × 48. |
| dump-soil | M3 | 0 | `records=5 start=1 soil=4 soil_valid=4 bad=0 sequence_breaks=0 partial_tail_bytes=0 result=PASS`. File is 240 B = 5 × 48. |
| cap-crosscheck | M3 | 0 | Light: 19 samples logged, 20 stored and 20 in the file. Soil: 4, 5 and 5. The difference of one is the START record. 0 non-`LIVE` lines. |
| cap-refuse | M3 | 1 | A second light run into the same directory was refused (`store_error op=open result=SEQUENCE`). The log's SHA-256 was identical before and after. |

## Stored readings

These come from `dump-light.out` and `dump-soil.out`. Every record has `simulated=0`, `clock_untrusted=0`, `time_unavailable=0`, `crc=OK`, and zero I2C status, stage and errno.
- **Light (19 records):** 10000 mlx ×10, 10833 mlx ×8, 9167 mlx ×1. These are uncalibrated (datasheet typical factor), so compare them with each other only.
- **Soil (4 records):** 805, 854, 828 and 802 raw counts. These are uncalibrated.

## Flushing

According to the `store_flush` lines:
- **Light:** after the START flush, batches of 6 records were flushed on the 60 s deadline.
- **Soil:** each record was flushed alone on the deadline.
- **On stop:** both processes flushed their last record when they closed.

The `store_flush` lines have no timestamps, so the saved files do not show each record's exact wait. The 60 s bound is checked by `test_capture_store` with a fake clock, not measured here.

## Deviations and limits

- **Not done:**
  - Step 1 (the powered-off wiring check, jumper record and photos) was not done.
  - Steps 5–7 were not run.
- **Physical setup unknown.** Light level and probe position were not observed or changed, so the values show only that readings were taken and stored, not how the sensors respond.
- **Clock trust.** `capture_linux` reported `clock_trust=TRUSTED`: the Pi's clock was synchronized this boot over the network used for SSH. `sensor-diag` always prints `clock_sync=unknown` by design. With networking disconnected, the design expects `CLOCK_UNTRUSTED` records; that case was not tested here.
- **Rates.** The capture rates are the PROPOSED defaults (10 s tick; light every tick, soil every 6th), not agreed requirements.
- **Not established:** calibration, long-term stability (the 48-hour test), behaviour with a sensor disconnected while capturing, recovery after a crash, and hub-wide sequencing.
- **Earlier run.** A preliminary folder from earlier the same day, `run-20261028T205455Z` (steps 2–4 on `main` at `8c9d661`, PASS 6/6), is kept privately and not summarized here.
