# Backup — 2026-10-17

Snapshot of the working firmware taken before adding the electromagnet relay to
`drive_test`. Also tagged in git as `backup-2026-10-17`
(`git checkout backup-2026-10-17 -- embedded/` restores it exactly).

| Sketch | State at backup |
|---|---|
| `drive_test/` | Keyboard drive bring-up + INA219 current stream (known-good bench tool) |
| `drive_calib/` | Locked-rotor + flat-ground calibration for incline compensation |
| `web_drive/` | MQTT/TLS firmware — verified on hardware against the EC2 broker 2026-10-13 |

`web_drive/secrets.h` is deliberately **not** copied (credentials; gitignored only at its
original path). Regenerate it from `secrets.example.h` if restoring from here.
