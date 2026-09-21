# Room Comfort Monitor: team rules

## Commands

- Build: `make`
- Tests: `make test`
- Sanitizers: `make asan`
- Valgrind: `make memcheck`
- Clean: `make clean`
- A change is DONE only when build, tests, and applicable sanitizer checks pass.
  Show the real command output.

## Hard constraints

- The product makes no external network calls. A future LAN-only interface is
  permitted, but the device must work with the network disconnected.
- No LLM API, cloud inference, or pretrained model may run in the product.
- Product intelligence must be code the team wrote and can defend.
- The project uses at least two physically distinct, cooperating sensors.
- Replayed or generated data must be labeled `SIMULATED` or `REPLAYED` in every
  output, log, report, and demonstration where it appears.
- Every daemon must be supervisable: clean exit status, no orphaned file
  descriptors after restart, and a heartbeat within 60 seconds of startup.
- Check every allocation and handle and log every system-call error path.
- Never weaken, skip, or delete a test merely to make the suite pass.
- Never invent measurements, logs, sensor output, or test results.

## Ownership

- TODO(team): Partner 1 owns `src/sensors/` and hardware-facing capture code.
- TODO(team): Partner 2 owns `src/storage/` and `src/analysis/`.
- Shared and reviewed by both: supervisor, interface, tests, and documentation.
- Ownership means first authorship and defense responsibility, not exclusive
  permission. Each milestone diff receives both agent and human partner review.

## Style

- Systems core: C17 with `-Wall -Wextra -Werror -pedantic`; no VLAs.
- Use cleanup labels for functions managing multiple resources.
- Keep functions focused and name units in identifiers where practical.
- Python is allowed only in `tools/` or a future interface directory. No graded
  operating-system mechanism may be hidden in Python.
- Make the smallest relevant change; do not refactor unrelated code.

## Workflow

- Explore the relevant files before proposing edits.
- For multi-file or algorithmic changes, write a plan and wait for team approval.
- Prefer tests before implementation and evidence over assertions.
- For hardware bugs, record direct evidence such as `dmesg`, timing captures,
  `/proc/interrupts`, and exact logs before proposing a fix.
- Commit only from a green state with messages in the form `M<n>: <what>`.
- Each partner works from a separate clone and personal Claude Code sessions.
- Copy raw `.jsonl` files from `~/.claude/projects/` at every M1-M5 milestone.
- Keep 6-10 annotated episodes in each partner's `PROMPTLOG.md` by M5.

