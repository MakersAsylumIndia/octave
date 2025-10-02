#include <Wire.h>
#include <Adafruit_DRV2605.h>

#define TCA9548A_ADDR 0x70

Adafruit_DRV2605 drv3;
Adafruit_DRV2605 drv5;
Adafruit_DRV2605 drv7;

void tcaselect(uint8_t channel) {
  if (channel > 7) return; 
  Wire.beginTransmission(TCA9548A_ADDR);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

void setup() {
  Serial.begin(9600);
  Wire.begin();

  // Initialize drv3 on channel 3
  tcaselect(3);
  if (!drv3.begin()) {
    Serial.println("Could not find DRV2605 on channel 3");
    while (1);
  }
  drv3.selectLibrary(1);
  drv3.setMode(DRV2605_MODE_INTTRIG);

  // Initialize drv5 on channel 5
  tcaselect(5);
  if (!drv5.begin()) {
    Serial.println("Could not find DRV2605 on channel 5");
    while (1);
  }
  drv5.selectLibrary(1);
  drv5.setMode(DRV2605_MODE_INTTRIG);

  // Initialize drv7 on channel 7
  tcaselect(7);
  if (!drv7.begin()) {
    Serial.println("Could not find DRV2605 on channel 7");
    while (1);
  }
  drv7.selectLibrary(1);
  drv7.setMode(DRV2605_MODE_INTTRIG);
}

void loop() {
  // play X effect on channel 3
  tcaselect(3);
  drv3.setWaveform(0, 36);  
  drv3.setWaveform(1, 0);
  drv3.go();

  // play X effect on channel 5
  tcaselect(5);
  drv5.setWaveform(0, 49);  
  drv5.setWaveform(1, 0);
  drv5.go();

  // play X effect on channel 7
  tcaselect(7);
  drv7.setWaveform(0, 70); 
  drv7.setWaveform(1, 0);
  drv7.go();

  delay(2000); // Let the effects play out
}