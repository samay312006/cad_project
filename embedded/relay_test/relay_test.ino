// relay_test.ino - standalone relay input diagnostic (bench tool, not part of web_drive).
// Send over serial:  0 = drive D23 LOW   1 = drive D23 HIGH   z = release D23 (high-Z input)
// Each command prints what it did. Note green/red LED, click, and magnet per state.
#define RELAY_PIN 23
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, INPUT);   // start released: the module's own pull-up decides
  Serial.println("relaytest ready. 0=LOW  1=HIGH  z=release(high-Z)");
}
void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == '0')      { pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW);  Serial.println("D23 driven LOW"); }
  else if (c == '1') { pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, HIGH); Serial.println("D23 driven HIGH (3.3V)"); }
  else if (c == 'z') { pinMode(RELAY_PIN, INPUT);                                  Serial.println("D23 released (high-Z)"); }
}
