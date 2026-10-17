// mix.js - pure joystick -> differential-drive duty mixing.
// dx, dy in [-1,1] (dx: left/right, dy: forward/back). speedCap in [0,1].
// Returns {left, right}, each an integer in [-255,255]. Loadable both as a
// plain <script> (defines a global) and as a Node module (for mix.test.js).

// Below roughly this duty the motors only hum/stall instead of turning
// (bench: duty 30 stalls, 60 spins, wheels off - HANDOFF.md section 2). Any
// non-zero command is lifted to at least this, then scaled up to the cap, so
// small stick deflections still move the robot. The firmware separately
// clamps duty to its motor-protection ceiling (drive_core.h dutyMaxCounts),
// so this can never push the motors past their rated voltage.
const MIN_DUTY = 70;

function mixDrive(dx, dy, speedCap) {
  const cap = Math.max(0, Math.min(1, speedCap));
  const ceiling = Math.round(255 * cap);
  const floor = Math.min(MIN_DUTY, ceiling);
  const side = raw => {
    const mag = Math.min(1, Math.abs(raw));
    if (mag === 0) return 0;
    const duty = Math.round(floor + mag * (ceiling - floor));
    return raw < 0 ? -duty : duty;
  };
  return { left: side(dy + dx), right: side(dy - dx) };
}

if (typeof module !== "undefined") module.exports = { mixDrive, MIN_DUTY };
