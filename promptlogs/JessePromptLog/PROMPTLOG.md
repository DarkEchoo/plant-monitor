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
