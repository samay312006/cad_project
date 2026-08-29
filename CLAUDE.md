# Project instructions (shared, read by every Claude Code session in this repo)

This is a two-person, two-Claude-Code-session project. This file is committed and shared —
both engineers' sessions read it. It does not by itself say which role *this* session plays;
that comes from a local, uncommitted `CLAUDE.local.md` (see below).

## Project

Ground robot + cloud control system. Full design lives in:
- `spec.md` — requirements, interfaces, sensor list, acceptance criteria
- `plan.md` — phased build plan (2a local-first sim → 2b AWS → 2c integration)
- `overview_controls.md` / `overview_cloud.md` — architecture summaries
- `ONBOARDING.md` — role split, boundaries, how to get set up
- `UPDATES.md` — **read this every session** — live cross-team blockers/dependencies

## The two roles

1. **Embedded/Firmware engineer** — owns everything on the ESP32: control loops, mode
   state machine + SAFE_HOLD, sensor/servo/motor drivers, the ESP32-side of the wireless
   link, the local HAL-based sim of the control core, and ESP32-side hardware bring-up.
2. **Cloud/GUI engineer** — owns AWS/EC2, SLAM Toolbox + Nav2 + D*-Lite, the MQTT broker
   (local Docker and EC2), and the Android phone app (GUI + phone-side Wi-Fi client +
   phone-side MQTT client).

Full detail, exact file/directory ownership, and hard limitations for each role are in
`ONBOARDING.md`. Determine which role *this* session is from `CLAUDE.local.md` in your own
checkout (gitignored, not shared — each engineer creates their own). If it doesn't exist yet,
create it from the template in `ONBOARDING.md` before writing any code, and ask the human
which role applies if it's genuinely unclear.

## Rules that apply to BOTH roles, no exceptions

- **Never create a real AWS resource, spend money, or run a cloud command without the
  human's explicit confirmation first** — this holds regardless of which engineer/session
  is doing it. Plan 2b is a written plan until the human says otherwise.
- **Local-first, always.** Every subsystem is validated in local simulation before it
  depends on real hardware or a real AWS resource. Don't propose or default to cloud-first.
- **`spec.md` §6 (interface contracts) is the shared source of truth for wire formats.**
  Neither role changes a shared message schema, topic, or protocol unilaterally — update
  `spec.md` and add a row to `UPDATES.md` so the other engineer sees it before code depends
  on the old shape.
- **Don't write code outside your role's ownership.** If a task seems to require touching
  the other side (e.g. embedded touching GUI code, or cloud/GUI touching ESP32 firmware),
  stop and flag it in `UPDATES.md` instead of just doing it.
- **Read `UPDATES.md` at the start of every session**, and add/resolve rows there whenever
  your work creates or clears a dependency the other engineer needs to know about.
