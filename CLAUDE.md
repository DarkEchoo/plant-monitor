# Plant Monitor: team rules

## Commands

- Build: `make`
- Tests: `make test`
- Sanitizers: `make asan`
- Valgrind: `make memcheck`
- Clean: `make clean`
- A change is DONE only when build, tests, and applicable sanitizer checks pass.
  Show the actual command output.

## Hardware contract

- Target: Raspberry Pi 3 Model B+ with its soldered 40-pin GPIO header.
- Soil sensor: Adafruit STEMMA Soil Sensor with a JST-PH 2 mm connector.
- Light sensor: Adafruit BH1750 with STEMMA QT/JST-SH connectors.
- Planned cables: two Adafruit 4397 cables and one Adafruit 4424
  STEMMA QT-to-GPIO cable.
- Both sensors share the Raspberry Pi's I2C bus and use 3.3 V power.
- Verify the connector path and pin labels against the actual components
  before connecting them. Do not assume a cable fits based on its name.
- Power off the Raspberry Pi before connecting or disconnecting hardware.
- Confirm labels and physical pins instead of trusting wire color alone.
- Real readings must include timestamps, units, validity, and error status.

## Hard constraints

- The product makes no external network calls. A future LAN-only interface is
  permitted, but the device must operate with networking disconnected.
- No LLM API, cloud inference, or pretrained model may run in the product.
- Product intelligence must be code the team wrote and can defend.
- Light readings help determine whether the chosen plant receives enough
  sunlight. Soil-moisture readings help determine whether it needs water.
  Both sensors feed one analysis pipeline and one set of plant-care advice.
- Simulated or replayed data must be visibly labeled in every output and log.
- Every daemon must be supervisable, with clean exit status, no orphaned file
  descriptors, and a heartbeat within 60 seconds of startup.
- Check allocations and handle and log every system-call error path.
- Never weaken, skip, or delete a test merely to make the suite pass.
- Never invent measurements, calibration results, logs, or test outcomes.

## Ownership

- Jesse owns `src/sensors/` and I2C capture.
- Michael owns `src/storage/` and `src/analysis/`.
- Shared and reviewed by both: supervisor, interface, tests, and documentation.
- Ownership means first authorship and defense responsibility, not exclusivity.

## Style

- Systems core: C17 with `-Wall -Wextra -Werror -pedantic`; no VLAs.
- Use cleanup labels for functions managing multiple resources.
- Include measurement units in names where practical.
- Python is allowed only in `tools/` or a future interface directory. No graded
  operating-system mechanism may be hidden in Python.
- Make the smallest relevant change; do not refactor unrelated code.
- Mark each function and label each new function to ensure a clean workflow.

## Workflow

- Explore relevant files before editing.
- For multi-file or algorithmic changes, plan first and wait for team approval.
- Prefer tests before implementation and evidence over assertions.
- For hardware bugs, record direct evidence such as `i2cdetect`, `dmesg`, exact
  error logs, and wiring observations before proposing a fix.
- Commit only from a green state with messages in the form `M<n>: <what>`.
- Each partner works from a separate clone and personal Claude Code sessions.
- Copy raw `.jsonl` files from `~/.claude/projects/` at every M1-M5 milestone.
- Each milestone diff receives fresh-context agent review and human partner review.
