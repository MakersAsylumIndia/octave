
#include <Wire.h>

#define TCA9548A_ADDR 0x70

// Channels to check
const uint8_t channels[] = {2, 3, 5, 7};  
const uint8_t numChannels = sizeof(channels) / sizeof(channels[0]);

// OLED typical I2C address
#define OLED_ADDR 0x3C

// Function to select TCA9548A channel
void tcaselect(uint8_t channel) {
  if (channel > 7) return;
  Wire.beginTransmission(TCA9548A_ADDR);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

// Check for device presence at I2C address on currently selected mux channel
bool checkDevice(uint8_t devAddr) {
  Wire.beginTransmission(devAddr);
  return (Wire.endTransmission() == 0);
}

// Check if a device at devAddr responds as DRV2605L by reading STATUS register (0x00)
bool checkDRV2605L(uint8_t devAddr) {
  Wire.beginTransmission(devAddr);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  Wire.requestFrom(devAddr, (uint8_t)1);
  if (Wire.available()) {
    uint8_t status = Wire.read();
    return ((status & 0xE0) == 0xE0);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  delay(1000);
  Serial.println("Starting I2C device scan on TCA9548A channels 2,3,5,7");

  for (uint8_t i=0; i < numChannels; i++) {
    uint8_t channel = channels[i];
    Serial.print("Selecting channel ");
    Serial.println(channel);
    tcaselect(channel);

    if(channel == 2) {
      // OLED channel: check for OLED address 0x3C
      if (checkDevice(OLED_ADDR)) {
        Serial.println("OLED display detected at 0x3C");
      } else {
        Serial.println("No OLED detected on channel 2");
      }
    } else {
      // DRV2605L channels: scan addresses 0x40-0x5F (common DRV2605L range) and confirm DRV2605L
      bool foundDRV = false;
      for (uint8_t addr=0x40; addr<=0x5F; addr++) {
        if (checkDevice(addr) && checkDRV2605L(addr)) {
          Serial.print("DRV2605L motor driver found on channel ");
          Serial.print(channel);
          Serial.print(" at address 0x");
          if (addr < 16) Serial.print("0");
          Serial.println(addr, HEX);
          foundDRV = true;
        }
      }
      if (!foundDRV) {
        Serial.print("No DRV2605L found on channel ");
        Serial.println(channel);
      }
    }
    Serial.println();
  }
}

void loop() {
  // Nothing to do here.
}