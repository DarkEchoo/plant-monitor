# Jesse's prompt log

Add 6-10 annotated episodes by M5. Include at least one revised plan, one real-
hardware failure, one review of your partner's work, and one agent-introduced
bug that you caught.

## Episode: review of Michael's PR #5 (design and storage plan), 2026-10-24

- **PR:** https://github.com/DarkEchoo/plant-monitor/pull/5, "Michael/storage design", head `8b51388`, documentation only (`docs/design.md`).
- **My decision:** request changes on the design.
  - Define validity by the driver's overall success, not by the I2C status alone.
  - Preserve the driver's failure stage and, when present, the rejected value. The proposed byte layout is optional.
  - Keep heartbeats running during retry backoff.
  - Specify that timestamps are taken at capture time, and define exactly what they mean.
  - Define when the clock is trusted while offline, and keep storage bounded then. Skipping age-based deletion must not allow unlimited disk growth.
- **Optional nits:** CRC variant, magic value, millilux wording.
- **Deferred:** when to re-run sensor initialization will be agreed during the capture-process design.

## Episode: agent-introduced test gap in sensor-diag clock order, 2026-10-24

- **Change:** sensor-diag (PR #8, commit `201064f`). Claude wrote the code and the tests.
- **Intended behaviour:** read both clocks immediately *after* each measurement attempt, including failed ones, so the timestamps mark when the attempt completed.
- **Gap:** the code did read the clocks after each attempt, but no test checked the order. The fresh-context reviewer agent found this: in a scratch copy it moved both clock reads before the measurement, and the whole suite still passed. Claude then reproduced the result.
- **Fix:** sequence assertions. The fake clock records how many mock script steps had run at each clock read, and the tests check that the measurement's steps ran first, for successful, failed and rejected attempts.
- **Evidence:**
  - With the clocks moved before the measurement, the suite then failed (`test_all_ok_exact_output`).
  - The real code passed the recorded checks: `make clean`, `make`, `make test`, `make asan`, `make memcheck` and `make test` all exited 0.
- **My role:** I reviewed the reviewer's report and asked for this entry. I did not find the gap myself.
