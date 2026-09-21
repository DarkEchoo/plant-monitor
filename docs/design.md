# Design document outline (M2)

This is an outline, not a submitted design document.

## Architecture

TODO: Add processes, threads, kernel interfaces, IPC, and rates on every arrow.

## Mechanism mapping

- D: append-only log, batching, `fsync` policy, and crash recovery experiment.
- E: separate sensor processes, Unix-domain socket hub, and supervising parent.

## Failure-mode table

| Component | Failure | Detection | Response | Log evidence |
|---|---|---|---|---|
| DHT22 capture | Sensor unplugged/checksum failure | Repeated read errors | Mark environmental data unavailable; retry with backoff | Error count and degraded-state transition |
| PIR capture | Process killed or GPIO unavailable | Child exit or missed heartbeat | Preserve environmental capture and restart child | Exit status, backoff, restart |
| Storage | Interrupted/short write | Return value and recovery scan | Discard incomplete tail record | Recovery count and byte offset |

## Storage and data

TODO: Define sample rates, record format, retention, batching, and power-loss behavior.

## Constraints and substitutions

TODO: Record the ideal components, the hardware actually ordered, and the cost
of each substitution. Label all simulated or replayed data.

## Evaluation plan

TODO: Set targets for event-to-hub latency, CPU/RSS, restart time, log recovery,
and the required 48-hour live-sensor soak test.

## Ownership map

TODO: Replace the placeholders in `CLAUDE.md` with both partners' names.

## AI-use plan

Claude Code may help plan, explain APIs, review diffs, and propose tests. It may
not run in the device or supply runtime decisions. The product source will
contain no external network client.

