# Updates & Blockers

Read this at the start of every session, both engineers. It's a pointer list, not a
discussion log — details belong in `spec.md`/`plan.md` or your own commits/PRs.

## Open blockers

| # | Blocker | Blocks | Owner to resolve | Opened |
|---|---|---|---|---|
| 3 | Single-point range sensor + servo BOM (`[TBD]` in spec) not chosen — sensor sample time and servo slew speed are unknown, so achievable scan rate/resolution is unknown | Both — embedded can't finalize driver/timing code; cloud/GUI can't set realistic SLAM Toolbox scan-rate expectations or Follow-me acceptance criteria (`spec.md` §8 Follow-me/SLAM rows are explicitly left open pending this) | Human (BOM decision) | 2026-08-27 |
| 4 | MQTT broker TLS/auth credential format for EC2 (`plan.md` 2b.3) not yet defined | Cloud/GUI engineer only, not a cross-blocker yet — flagged so it doesn't get skipped before 2c integration | Cloud/GUI engineer | 2026-08-27 |

## Recently resolved

| # | Blocker | Resolution | Date |
|---|---|---|---|
| 5 | Current-sensor granularity (`spec.md` §4/§6.1) left `fl/fr/rl/rr` in the telemetry schema ambiguous about whether all 4 were independent readings | Resolved to 2x INA219, one per drive channel (left/right side, matching the skid-steer chassis) — `fl`==`rl` (left side) and `fr`==`rr` (right side) always report the same value. `spec.md` §4 (current sensor row) and §6.1 (telemetry example) updated with an explicit note. **Correction 2026-08-29**: an earlier version of this note/edit had the pairing backwards (`fl`==`fr`, front/rear) — fixed to the correct side-based pairing before it reached the diagram or any GUI work. If any GUI/dashboard work already assumed `fl`==`fr`, rebuild against the corrected schema. | 2026-08-29 |
| 1 | Wireless-link + sensor architecture wasn't yet applied to the docs | Applied to `spec.md` (§3, §4, §5, §6.1, §6.2, §6.3, §7, §8, §9), `plan.md` (2a.2, 2a.3, 2a.5, 2a.6, 2b.2, 2c.3, 2c.5), and both `overview_*.md` files. Both engineers can now code against the new interface. | 2026-08-27 |
| 2 | Packet framing for the ESP32↔phone Wi-Fi link wasn't defined | Defined in `spec.md` §6.1: UDP, one JSON message per datagram, `~1200 byte` MTU headroom, new `scan_sample` message type alongside existing telemetry/command messages. Still `[ASSUMPTION]`-tagged, not yet bench-verified, but the cloud/GUI engineer can build the phone-side client against it now. | 2026-08-27 |

## How to use this file

- Add a row the moment your work creates a dependency the other engineer needs to know
  about — don't wait for them to ask.
- When a blocker clears, move it to "Recently resolved" with a one-line note on how, and
  the date.
- Keep entries short. If it needs more than 2-3 sentences, put the detail in `spec.md` or
  a PR description and link to it here.
