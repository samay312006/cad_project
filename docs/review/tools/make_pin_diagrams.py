"""Draws the pin-connection diagrams used in section 3 of project_review_2.

Solid = wired (pins from embedded/web_drive/drive_core.h); dashed = proposed,
not yet wired. Run: python3 make_pin_diagrams.py <out_dir>
"""
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
NAVY = "#1a2b5c"
GREY = "#888888"
COL = {"sda": "#1f77b4", "scl": "#2ca02c", "adc": "#9467bd", "pwm": "#d62728",
       "dir": "#ff7f0e", "uart": "#8c564b", "pow": "#555555", "gate": "#e377c2", "bat": "#bb0000"}


def new(title, h):
    fig, ax = plt.subplots(figsize=(14, h), dpi=130)
    ax.set_xlim(0, 14)
    ax.set_ylim(0, h)
    ax.axis("off")
    ax.text(7, h - 0.15, title, ha="center", va="top", fontsize=15, weight="bold", color=NAVY)
    return fig, ax


def box(ax, x, y, w, h, title, sub=None, proposed=False):
    ec = GREY if proposed else NAVY
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.1",
                                fc="#fbfbfb" if proposed else "#eef2fb", ec=ec, lw=2,
                                ls="--" if proposed else "-"))
    ax.text(x + w / 2, y + h - 0.2, title, ha="center", va="top", fontsize=11, weight="bold",
            color="#555" if proposed else NAVY)
    if sub:
        ax.text(x + w / 2, y + h - 0.58, sub, ha="center", va="top", fontsize=8.5, color="#444")


def line(ax, xs, ys, color, proposed=False, lw=2.2):
    ax.plot(xs, ys, color=color, lw=lw, ls="--" if proposed else "-", solid_capstyle="round")


def esp_pin(ax, x, y, label):
    ax.text(x - 0.15, y, label, ha="right", va="center", fontsize=9.5)
    ax.plot(x, y, "s", color=NAVY, ms=7)


def dev_pin(ax, x, y, label, side="R"):
    ax.plot(x, y, "o", color=NAVY, ms=5)
    ax.text(x + (0.15 if side == "R" else -0.15), y, label, ha="left" if side == "R" else "right",
            va="center", fontsize=9)


def legend(ax, y, items):
    x = 0.5
    for name, col, ls in items:
        ax.plot([x, x + 0.6], [y, y], color=col, lw=3, ls=ls)
        ax.text(x + 0.7, y, name, va="center", fontsize=9)
        x += 1.2 + 0.13 * len(name)


def save(fig, name):
    fig.savefig(f"{OUT}/{name}", bbox_inches="tight", facecolor="white")
    plt.close(fig)


# ------------------------------------------------------------ Fig A: I2C sensors
fig, ax = new("Fig. A - I2C sensor connections (shared bus on GPIO21 / GPIO22)", 6.6)
box(ax, 0.6, 0.9, 3.0, 4.7, "ESP32 DevKit", "ESP32-D0WD-V3")
esp_pin(ax, 3.6, 4.5, "GPIO21 (SDA)")
esp_pin(ax, 3.6, 3.7, "GPIO22 (SCL)")
esp_pin(ax, 3.6, 2.2, "3V3 (to every VCC)")
esp_pin(ax, 3.6, 1.5, "GND (to every GND)")
bus_sda, bus_scl = 5.0, 5.5
line(ax, [3.6, bus_sda, bus_sda], [4.5, 4.5, 1.0], COL["sda"])
line(ax, [3.6, bus_scl, bus_scl], [3.7, 3.7, 1.0], COL["scl"])
ax.text(5.25, 5.35, "I2C bus", ha="center", fontsize=9, weight="bold")
devs = [("INA219 #1 (left motor current)", "I2C address 0x40", 4.45, False),
        ("INA219 #2 (right motor current)", "I2C address 0x41", 3.2, False),
        ("IMU (MPU-6050 class)", "I2C address 0x68", 1.95, True),
        ("ToF range sensor", "I2C address 0x29", 0.7, True)]
for name, sub, yy, prop in devs:
    box(ax, 8.4, yy - 0.05, 4.2, 1.1, name, sub, proposed=prop)
    y1, y2 = yy + 0.7, yy + 0.4
    line(ax, [bus_sda, 8.4], [y1, y1], COL["sda"], prop, 1.8)
    line(ax, [bus_scl, 8.4], [y2, y2], COL["scl"], prop, 1.8)
    ax.plot(bus_sda, y1, "o", color=COL["sda"], ms=5)
    ax.plot(bus_scl, y2, "o", color=COL["scl"], ms=5)
    ax.text(7.2, y1 + 0.08, "SDA", ha="center", va="bottom", fontsize=8, color=COL["sda"])
    ax.text(7.2, y2 - 0.08, "SCL", ha="center", va="top", fontsize=8, color=COL["scl"])
    ax.text(12.7, yy + 0.5, "VCC <- 3V3\nGND <- GND", va="center", fontsize=8, color=COL["pow"])
ax.text(10.5, 5.9, "motor current: shunt in each channel's return lead", ha="center", fontsize=8.5, style="italic")
legend(ax, 0.05, [("SDA", COL["sda"], "-"), ("SCL", COL["scl"], "-"), ("wired", "#222", "-"),
                  ("proposed, not wired", GREY, "--")])
ax.set_ylim(-0.2, 6.6)
save(fig, "pin_sensors_i2c.png")

# ------------------------------------------------- Fig B: Vbus divider
fig, ax = new("Fig. B - Bus-voltage divider to the ESP32 ADC (GPIO34)", 6.3)
box(ax, 0.6, 0.7, 3.0, 3.8, "ESP32 DevKit", "ESP32-D0WD-V3")
esp_pin(ax, 3.6, 2.9, "GPIO34 (ADC)")
esp_pin(ax, 3.6, 1.0, "GND")
x = 7.0
ax.text(x, 5.0, "Battery +  (about 12 V)", ha="center", fontsize=9.5, color=COL["bat"], weight="bold")
line(ax, [x, x], [4.8, 4.2], COL["bat"])
ax.add_patch(Rectangle((x - 0.22, 3.4), 0.44, 0.8, fc="white", ec=COL["adc"], lw=2))
ax.text(x + 0.4, 3.8, "R1  100 k", va="center", fontsize=9.5)
line(ax, [x, x], [3.4, 2.9], COL["adc"])
line(ax, [3.6, x], [2.9, 2.9], COL["adc"])
ax.plot(x, 2.9, "o", color=COL["adc"], ms=7)
line(ax, [x, x], [2.9, 2.5], COL["adc"])
ax.add_patch(Rectangle((x - 0.22, 1.7), 0.44, 0.8, fc="white", ec=COL["adc"], lw=2))
ax.text(x + 0.4, 2.1, "R2  22 k", va="center", fontsize=9.5)
line(ax, [x, x], [1.7, 1.0], COL["pow"])
cx = x + 2.0
line(ax, [x, cx], [2.9, 2.9], COL["adc"])
line(ax, [cx, cx], [2.9, 2.35], COL["adc"])
ax.plot([cx - 0.28, cx + 0.28], [2.35, 2.35], color=COL["adc"], lw=3)
ax.plot([cx - 0.28, cx + 0.28], [2.1, 2.1], color=COL["adc"], lw=3)
line(ax, [cx, cx], [2.1, 1.0], COL["pow"])
ax.text(cx + 0.4, 2.22, "100 nF (PWM ripple filter)", va="center", fontsize=9.5)
line(ax, [3.6, cx], [1.0, 1.0], COL["pow"])
ax.text(x - 3.4, 0.45, "Ratio 5.545: 12.6 V reads about 2.27 V at GPIO34, below the 3.3 V ADC limit",
        ha="left", fontsize=9, style="italic")
legend(ax, -0.15, [("ADC signal", COL["adc"], "-"), ("battery +", COL["bat"], "-"), ("GND", COL["pow"], "-")])
ax.set_ylim(-0.4, 6.3)
save(fig, "pin_vbus_divider.png")

# ------------------------------------------------- Fig B2: GPS
fig, ax = new("Fig. B2 - GPS module on UART2 (proposed, not yet wired)", 4.4)
box(ax, 0.6, 0.6, 3.0, 2.8, "ESP32 DevKit", "ESP32-D0WD-V3")
esp_pin(ax, 3.6, 2.2, "GPIO16 (RX2)")
esp_pin(ax, 3.6, 1.5, "GPIO17 (TX2)")
esp_pin(ax, 3.6, 0.9, "3V3 / GND")
box(ax, 8.0, 0.7, 3.6, 2.6, "GPS module (UART)", "NMEA, 9600 baud class", proposed=True)
line(ax, [3.6, 8.0], [2.2, 2.2], COL["uart"], True)
line(ax, [3.6, 8.0], [1.5, 1.5], COL["uart"], True)
line(ax, [3.6, 8.0], [0.9, 0.9], COL["pow"], True)
for yy, lab in [(2.2, "TX"), (1.5, "RX"), (0.9, "VCC / GND")]:
    ax.plot(8.0, yy, "o", color=NAVY, ms=5)
    ax.text(8.2, yy, lab, va="center", fontsize=9)
ax.text(5.8, 2.32, "GPS TX -> ESP32 RX2", ha="center", fontsize=8.5, color=COL["uart"])
ax.text(5.8, 1.62, "ESP32 TX2 -> GPS RX", ha="center", fontsize=8.5, color=COL["uart"])
ax.set_ylim(0, 4.4)
save(fig, "pin_gps.png")

# ------------------------------------------------------------- Fig C: actuators
fig, ax = new("Fig. C - Actuator connections (motor driver, sweep servo, electromagnet)", 8.0)
box(ax, 0.6, 0.25, 3.0, 6.75, "ESP32 DevKit", "ESP32-D0WD-V3")
rows = [("GPIO25", "ENA", 6.0, "pwm", False), ("GPIO26", "IN1", 5.5, "dir", False),
        ("GPIO27", "IN2", 5.0, "dir", False), ("GPIO32", "ENB", 4.2, "pwm", False),
        ("GPIO33", "IN3", 3.7, "dir", False), ("GPIO14", "IN4", 3.2, "dir", False),
        ("GPIO18", "SIG", 2.1, "pwm", True), ("GPIO19", "GATE", 0.7, "gate", True)]
box(ax, 6.2, 2.9, 2.8, 4.1, "L298N driver", "dual H-bridge")
for g, name, yy, col, prop in rows:
    esp_pin(ax, 3.6, yy, g)
    if not prop:
        line(ax, [3.6, 6.2], [yy, yy], COL[col])
        dev_pin(ax, 6.2, yy, name)
box(ax, 6.2, 1.85, 2.8, 0.95, "Sweep servo", proposed=True)
line(ax, [3.6, 6.2], [2.1, 2.1], COL["pwm"], True)
dev_pin(ax, 6.2, 2.1, "SIG")
box(ax, 6.2, 0.25, 2.8, 1.35, "Electromagnet", "MOSFET switch + flyback diode across coil", proposed=True)
line(ax, [3.6, 6.2], [0.7, 0.7], COL["gate"], True)
dev_pin(ax, 6.2, 0.7, "GATE")
box(ax, 10.6, 5.0, 3.0, 1.8, "Left motors", "FL + RL in parallel (channel A)")
box(ax, 10.6, 3.0, 3.0, 1.8, "Right motors", "FR + RR in parallel (channel B)")
line(ax, [9.0, 10.6], [5.9, 5.9], "#222", lw=3)
line(ax, [9.0, 10.6], [3.9, 3.9], "#222", lw=3)
ax.text(9.8, 6.05, "OUT1 / OUT2", ha="center", fontsize=8.5)
ax.text(9.8, 4.05, "OUT3 / OUT4", ha="center", fontsize=8.5)
ax.text(9.6, 1.2, "Power: L298N motor supply from the 12 V battery;\nESP32 and servo from a 5 V buck; common GND",
        ha="left", va="center", fontsize=8.5, style="italic", color=COL["pow"])
legend(ax, 0.0, [("PWM", COL["pwm"], "-"), ("Direction", COL["dir"], "-"), ("MOSFET gate", COL["gate"], "--"),
                 ("wired", "#222", "-"), ("proposed, not wired", GREY, "--")])
ax.set_ylim(-0.3, 8.0)
save(fig, "pin_actuators.png")
print("ok")
