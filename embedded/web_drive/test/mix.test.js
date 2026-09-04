// embedded/web_drive/test/mix.test.js
const assert = require("assert");
const { mixDrive } = require("../data/mix.js");

// straight forward, full stick, full speed
{
  const { left, right } = mixDrive(0, 1, 1);
  assert.strictEqual(left, 255);
  assert.strictEqual(right, 255);
}

// spin right in place, full stick, full speed (matches drive_test.ino's
// 'd' key: drive(speed, -speed))
{
  const { left, right } = mixDrive(1, 0, 1);
  assert.strictEqual(left, 255);
  assert.strictEqual(right, -255);
}

// spin left in place (matches drive_test.ino's 'a' key: drive(-speed, speed))
{
  const { left, right } = mixDrive(-1, 0, 1);
  assert.strictEqual(left, -255);
  assert.strictEqual(right, 255);
}

// speed cap scales the output
{
  const { left, right } = mixDrive(0, 1, 0.5);
  assert.strictEqual(left, 128);
  assert.strictEqual(right, 128);
}

// centered stick -> zero, regardless of speed cap
{
  const { left, right } = mixDrive(0, 0, 1);
  assert.strictEqual(left, 0);
  assert.strictEqual(right, 0);
}

// output never exceeds +/-255 even with out-of-range inputs
{
  const { left, right } = mixDrive(1, 1, 1);
  assert.ok(left <= 255 && left >= -255);
  assert.ok(right <= 255 && right >= -255);
}

// speedCap is clamped to [0,1] even if called with an out-of-range value
{
  const { left, right } = mixDrive(0, 1, 2);
  assert.strictEqual(left, 255);
  assert.strictEqual(right, 255);
}
{
  const { left, right } = mixDrive(0, 1, -1);
  assert.strictEqual(left, 0);
  assert.strictEqual(right, 0);
}

// diagonal stick saturates to exact expected values, not just "within bounds"
{
  const { left, right } = mixDrive(1, 1, 1);
  assert.strictEqual(left, 255);
  assert.strictEqual(right, 0);
}

console.log("mix.test.js: all assertions passed");
