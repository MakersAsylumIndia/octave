#include <Wire.h>

#define TCA9548A_ADDR 0x70  // Default I2C address for TCA9548A multiplexer
#define TCA_CHANNEL 5       // Multiplexer channel where DRV2605L is connected

// Select the multiplexer channel
void tcaselect(uint8_t channel) {
  if (channel > 7) return;
  Wire.beginTransmission(TCA9548A_ADDR);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  delay(1000);

  Serial.print("Selecting TCA9548A channel ");
  Serial.println(TCA_CHANNEL);
  tcaselect(TCA_CHANNEL);

  Serial.println("Scanning for DRV2605L on this channel...");

  // Scan all possible I2C addresses and check for DRV2605L
  bool found = false;
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("Device found at 0x");
      if (address < 16) Serial.print("0");
      Serial.print(address, HEX);

      // Try reading the STATUS register (0x00) to confirm DRV2605L
      Wire.beginTransmission(address);
      Wire.write(0x00);
      if (Wire.endTransmission() == 0) {
        Wire.requestFrom(address, (uint8_t)1);
        if (Wire.available()) {
          uint8_t status = Wire.read();
          if ((status & 0xE0) == 0xE0) {
            Serial.print(" -- DRV2605L confirmed!");
            found = true;
          }
        }
      }
      Serial.println();
    }
  }
  if (!found) {
    Serial.println("No DRV2605L device found on this channel.");
  }
}

void loop() {
  // Nothing here
}
