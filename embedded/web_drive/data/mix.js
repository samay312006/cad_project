// mix.js - pure joystick -> differential-drive duty mixing.
// dx, dy in [-1,1] (dx: left/right, dy: forward/back). speedCap in [0,1].
// Returns {left, right}, each an integer in [-255,255]. Loadable both as a
// plain <script> (defines a global) and as a Node module (for mix.test.js).
function mixDrive(dx, dy, speedCap) {
  const cap = Math.max(0, Math.min(1, speedCap));
  const clamp = v => Math.max(-255, Math.min(255, v));
  const left  = clamp(Math.round((dy + dx) * 255 * cap));
  const right = clamp(Math.round((dy - dx) * 255 * cap));
  return { left, right };
}

if (typeof module !== "undefined") module.exports = { mixDrive };
