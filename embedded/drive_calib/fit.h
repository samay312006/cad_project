#pragma once
#include <math.h>

// Least squares of y = intercept + slope*x, plus the RMS residual so a
// non-linear result is visible rather than silently averaged away.
//
// Lives in its own header because Arduino generates function prototypes above
// the sketch body, which would place `Fit fitLine(...)` before the struct.
struct Fit { float slope, intercept, rms; bool ok; };

inline Fit fitLine(const float *x, const float *y, int n) {
  Fit f{0, 0, 0, false};
  if (n < 2) return f;
  float sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (int i = 0; i < n; i++) { sx += x[i]; sy += y[i]; sxx += x[i]*x[i]; sxy += x[i]*y[i]; }
  float den = n * sxx - sx * sx;
  if (fabsf(den) < 1e-9f) return f;
  f.slope     = (n * sxy - sx * sy) / den;
  f.intercept = (sy - f.slope * sx) / n;
  float acc = 0;
  for (int i = 0; i < n; i++) {
    float e = y[i] - (f.intercept + f.slope * x[i]);
    acc += e * e;
  }
  f.rms = sqrtf(acc / n);
  f.ok  = true;
  return f;
}
