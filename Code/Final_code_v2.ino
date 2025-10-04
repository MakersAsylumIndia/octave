#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <arduinoFFT.h>
#include <Wire.h>
#include <U8g2lib.h>          // For SH1106 OLED display
#include <Adafruit_DRV2605.h> // For Haptic Drivers

// --- HARDWARE CONFIGURATION (ADJUSTED FOR XIAO ESP32 C3) ---
// I2C Multiplexer address and I2C setup
#define TCA9548A_ADDR 0x70
#define SDA_PIN 8 // XIAO C3 SDA pin (D8)
#define SCL_PIN 9 // XIAO C3 SCL pin (D9)

// Motor Driver objects
Adafruit_DRV2605 drv3; // Bass Motor on Channel 3
Adafruit_DRV2605 drv5; // Mids Motor on Channel 5
Adafruit_DRV2605 drv7; // Highs Motor on Channel 7

// SD Card Configuration
#define SD_CS_PIN 7           // XIAO C3 CS pin (D7)

// OLED Display (connected directly to primary I2C bus, not Mux)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// --- FFT & AUDIO CONFIGURATION ---
#define SAMPLES 1024          // Power of 2 (FFT size)
#define SAMPLING_FREQ 16000   // WAV sampling rate (must match your file's sample rate)
#define WAV_HEADER_SIZE 44    // Standard WAV header size in bytes
#define ANALYSIS_INTERVAL 200 // Analyze every 200 milliseconds for smooth feedback

// Frequency Resolution (dF) = 15.625 Hz/bin
#define MAX_FREQ_BIN 512      // Max useful bin is SAMPLES/2 (8000 Hz)

// Frequency Band Bins (Hertz to Bin Index Conversion: Hz / 15.625)
#define BASS_END_BIN   16     // ~250 Hz (Bins 1 to 16)
#define MIDS_END_BIN   64     // ~1000 Hz (Bins 17 to 64)
#define HIGHS_END_BIN  160    // ~2500 Hz (Bins 65 to 160) - Limits analysis to 2.5kHz
#define VISUAL_BINS    64     // Still visualize the first 64 bins for screen

// --- SCALING CONSTANT ---
// Set to 300000.0 based on your feedback to maximize 0-255 dynamic range.
#define MAX_BIN_MAGNITUDE 300000.0

// FFT arrays and object (must explicitly use <double>)
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT; 

// Global array to track current bar height for visual decay effect
float currentBarHeights[VISUAL_BINS] = {0.0}; 

// --- FILE & DATA MANAGEMENT ---
const char* songFiles[] = {
  "/1.wav",
  "/2.wav",
  "/3.wav" // Add all your WAV files here
};
const uint8_t NUM_SONGS = sizeof(songFiles) / sizeof(songFiles[0]);
int currentSongIndex = 0; // Index of the next song to open
int currentDisplayIndex = NUM_SONGS - 1; // Index of the currently playing song
File currentWavFile;
uint32_t lastAnalysisTime = 0;

// --- SERIAL DEBUG TIMING ---
const long SERIAL_PRINT_INTERVAL = 15000; // Print PWM values every 15 seconds (15000 ms)
uint32_t lastSerialPrintTime = 0;

// --- FUNCTION PROTOTYPES ---
void drawAnalyzer(double* magnitudes, size_t numBins);
bool openNextSong();
void analyzeAndDisplayAudio();

// --- I2C MULTIPLEXER CONTROL ---
void tcaselect(uint8_t channel) {
  if (channel > 7) return; 
  Wire.beginTransmission(TCA9548A_ADDR);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

// --- MOTOR CONTROL (IMPROVED DYNAMIC MAPPING) ---
void driveMotors(int bassPWM, int midsPWM, int highsPWM) {
    
    // Helper function to dynamically select a better effect based on intensity
    auto getDynamicEffect = [](int pwm) -> uint8_t {
      if (pwm < 10) return 0; // Very low or zero intensity
      
      if (pwm < 85) {
        // Low intensity: Subtle, crisp click (Library 1, Effects 1-4)
        return map(pwm, 10, 84, 1, 4); 
      } else if (pwm < 170) {
        // Medium intensity: Stronger, more prolonged rumble (Library 1, Effects 5-9)
        return map(pwm, 85, 169, 5, 9);
      } else {
        // High intensity: Powerful, sharp double click or buzz (Library 1, Effects 10-20)
        return map(pwm, 170, 255, 10, 20);
      }
    };
    
    // BASS (Channel 3)
    tcaselect(3);
    uint8_t bassEffect = getDynamicEffect(bassPWM);
    drv3.setWaveform(0, bassEffect);
    drv3.setWaveform(1, 0); // End sequence
    drv3.go();

    // MIDS (Channel 5)
    tcaselect(5);
    uint8_t midsEffect = getDynamicEffect(midsPWM);
    drv5.setWaveform(0, midsEffect);
    drv5.setWaveform(1, 0); 
    drv5.go();

    // HIGHS (Channel 7)
    tcaselect(7);
    uint8_t highsEffect = getDynamicEffect(highsPWM);
    drv7.setWaveform(0, highsEffect);
    drv7.setWaveform(1, 0); 
    drv7.go();
}


// --- SONG MANAGEMENT ---
// Function to open the next song in the array
bool openNextSong() {
  if (currentWavFile) {
    currentWavFile.close();
  }
  
  currentDisplayIndex = currentSongIndex; 

  Serial.print("\nLoading Song: "); Serial.println(songFiles[currentDisplayIndex]);
  currentWavFile = SD.open(songFiles[currentDisplayIndex]);
  
  if (!currentWavFile) {
    Serial.print("ERROR: Could not open "); Serial.println(songFiles[currentDisplayIndex]);
    return false;
  }
  
  currentWavFile.seek(WAV_HEADER_SIZE); // Skip WAV header
  currentSongIndex = (currentSongIndex + 1) % NUM_SONGS; // Cycle index for next song
  
  return true;
}

// --- OLED VISUALIZATION ---
void drawAnalyzer(double* magnitudes, size_t numBins) {
  u8g2.clearBuffer();
  
  // Display current song name at the top
  u8g2.setFont(u8g2_font_5x7_tr); 
  u8g2.drawStr(0, 7, "Now Playing:");
  u8g2.drawStr(60, 7, songFiles[currentDisplayIndex] + 1); // +1 skips the leading '/'
  
  const int maxBarHeight = 50; // Max height for the FFT bars
  const float decayRate = 3.0; // How fast the bars fall back down (pixels per frame)

  for (uint8_t i = 0; i < numBins; i++) {
    // 1. Calculate Target Height
    // Normalize magnitude for visual display (logarithmic scaling is often best for FFT)
    double magnitude = magnitudes[i + 1]; // Skip bin 0 (DC component)
    // We use a simple multiplier and constrain to fit the screen height.
    float targetHeight = constrain(log10(magnitude) * 12.0, 1.0, (float)maxBarHeight); 
    
    // 2. Apply Dynamic Decay Logic
    if (targetHeight > currentBarHeights[i]) {
      // Jump up instantly to the new peak
      currentBarHeights[i] = targetHeight;
    } else {
      // Slowly decay downwards
      currentBarHeights[i] -= decayRate;
      if (currentBarHeights[i] < 1.0) currentBarHeights[i] = 1.0; // Keep minimum visibility
    }
    
    // 3. Draw the bar
    int height = (int)currentBarHeights[i];
    // Draw the bar (i*2 gives a 2-pixel width per bar, 128 / 64 = 2)
    // Bars are drawn from the bottom up (y=64)
    u8g2.drawBox(i * 2, u8g2.getHeight() - height, 2, height); 
  }

  // Draw frequency boundary lines for reference
  // LOW_FREQ_END_HZ / (SAMPLING_FREQ / SAMPLES) * 2 = BASS_END_BIN * 2
  u8g2.drawVLine(BASS_END_BIN * 2, u8g2.getHeight() - 5, 5); // Line for 250Hz (Approx 16 bins * 2px/bin)
  u8g2.drawVLine(MIDS_END_BIN * 2, u8g2.getHeight() - 5, 5); // Line for 1000Hz (Approx 64 bins * 2px/bin)
  
  u8g2.sendBuffer(); // Send to display
}


// --- MAIN ANALYSIS AND CONTROL ---
void analyzeAndDisplayAudio() {
    
    // --- 1. Read Samples (16-bit Signed PCM) ---
    size_t i = 0;
    while (i < SAMPLES && currentWavFile.available()) {
        int16_t sample;
        size_t bytesRead = currentWavFile.read((uint8_t*)&sample, 2); 
        
        if (bytesRead == 2) {
            vReal[i] = (double)sample; 
            vImag[i] = 0.0;
            i++;
        } else {
            break; // Reached EOF or error reading the last byte
        }
    }
    
    if (i < SAMPLES) {
        Serial.println("--- End of Song reached. Restarting file... ---");
        if (!openNextSong()) {
            return; // Failed to open next song
        }
        return; // Skip analysis this loop, wait for next analysis interval
    }

    // --- 2. Perform FFT (Case-corrected functions) ---
    FFT.windowing(vReal, SAMPLES, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.compute(vReal, vImag, SAMPLES, FFT_FORWARD);
    FFT.complexToMagnitude(vReal, vImag, SAMPLES);

    // --- 3. Calculate Average Magnitudes for Haptic Bands ---
    double bassMag = 0.0, midsMag = 0.0, highsMag = 0.0;
    
    // BASS (Bins 1 to 16, 250 Hz)
    for (int k = 1; k <= BASS_END_BIN; k++) {
        bassMag += vReal[k];
    }
    bassMag /= BASS_END_BIN; // Calculate average
    
    // MIDS (Bins 17 to 64, 250 Hz to 1000 Hz)
    for (int k = BASS_END_BIN + 1; k <= MIDS_END_BIN; k++) {
        midsMag += vReal[k];
    }
    midsMag /= (MIDS_END_BIN - BASS_END_BIN);
    
    // HIGHS (Bins 65 to 160, 1000 Hz to 2500 Hz)
    for (int k = MIDS_END_BIN + 1; k <= HIGHS_END_BIN; k++) {
        highsMag += vReal[k];
    }
    highsMag /= (HIGHS_END_BIN - MIDS_END_BIN);

    // --- 4. Scale Magnitudes to PWM (0-255) ---
    auto scaleToPWM = [](double mag) {
        return constrain(
            map((long)mag, 0, (long)MAX_BIN_MAGNITUDE, 0, 255), 
            0, 
            255
        );
    };

    int bassPWM = scaleToPWM(bassMag);
    int midsPWM = scaleToPWM(midsMag);
    int highsPWM = scaleToPWM(highsMag);

    // --- 5. Conditional Debug Output: Print scaled PWM values (0-255) ---
    if (millis() - lastSerialPrintTime >= SERIAL_PRINT_INTERVAL) {
        lastSerialPrintTime = millis();
        
        Serial.println("--- HAPTIC PWM INTENSITIES (Printed Every 15s) ---");
        Serial.print("BASS (~250Hz): "); Serial.println(bassPWM);
        Serial.print("MIDS (~1kHz): "); Serial.println(midsPWM);
        Serial.print("HIGHS (~2.5kHz): "); Serial.println(highsPWM);
    }
    
    // --- 6. Drive Motors ---
    driveMotors(bassPWM, midsPWM, highsPWM);


    // --- 7. Display Visualization ---
    // Note: The visualization still shows only the first 64 bins (up to 1kHz) for screen clarity
    drawAnalyzer(vReal, VISUAL_BINS); 
}

// --- ARDUINO SETUP AND LOOP ---
void setup() {
    // Serial initialization for debugging
    Serial.begin(9600); 
    while (!Serial && millis() < 3000) { delay(10); }
    delay(500);         
    Serial.println("--- Starting Haptic Audio Analyzer with Visualizer (XIAO C3) ---");
    
    // 1. I2C Initialization
    Serial.print("1. Initializing I2C bus on XIAO pins D8/D9 (SDA/SCL)...");
    Wire.begin(SDA_PIN, SCL_PIN); 
    Serial.println("Done.");


    // 2. Check TCA9548A Multiplexer (MUST be done first)
    Serial.print("2. Checking TCA9548A (Mux) at 0x70...");
    Wire.beginTransmission(TCA9548A_ADDR);
    if (Wire.endTransmission() != 0) {
        u8g2.clearBuffer();
        u8g2.drawStr(0, 15, "FATAL ERROR:");
        u8g2.drawStr(0, 30, "MUX 0x70 Failed!");
        u8g2.sendBuffer();
        Serial.println("FATAL ERROR: TCA9548A (Mux) not found at 0x70. Check wiring/pull-ups.");
        while (1) delay(1000); 
    }
    Serial.println("Found!");


    // 3. Initialize Haptic Drivers (DRV2605)
    Serial.println("3. Initializing DRV2605 Drivers...");
    
    // DRV3 (BASS) on Channel 3
    tcaselect(3);
    if (!drv3.begin()) {
      Serial.println("FATAL ERROR: Could not find DRV2605 on channel 3 (BASS).");
      while (1) delay(1000); 
    }
    drv3.selectLibrary(1); // Select library 1 for simple effects
    drv3.setMode(DRV2605_MODE_INTTRIG); // Set to internal trigger mode
    Serial.println("DRV3 (BASS) initialized.");

    // DRV5 (MIDS) on Channel 5
    tcaselect(5);
    if (!drv5.begin()) {
      Serial.println("FATAL ERROR: Could not find DRV2605 on channel 5 (MIDS).");
      while (1) delay(1000); 
    }
    drv5.selectLibrary(1);
    drv5.setMode(DRV2605_MODE_INTTRIG);
    Serial.println("DRV5 (MIDS) initialized.");

    // DRV7 (HIGHS) on Channel 7
    tcaselect(7);
    if (!drv7.begin()) {
      Serial.println("FATAL ERROR: Could not find DRV2605 on channel 7 (HIGHS).");
      while (1) delay(1000); 
    }
    drv7.selectLibrary(1);
    drv7.setMode(DRV2605_MODE_INTTRIG);
    Serial.println("DRV7 (HIGHS) initialized.");


    // 4. OLED Initialization (Still on primary bus)
    Serial.println("4. Initializing U8g2 OLED...");
    u8g2.begin();
    u8g2.setFont(u8g2_font_ncenB08_tr); 
    u8g2.clearBuffer();
    u8g2.drawStr(0, 15, "XIAO C3 Ready!");
    u8g2.drawStr(0, 30, "Check SD Card!");
    u8g2.sendBuffer();
    Serial.println("OLED initialized.");

    // 5. SD Card Initialization
    Serial.print("5. Initializing SD Card on XIAO pin D7 (CS)...");
    if (!SD.begin(SD_CS_PIN)) {
        u8g2.clearBuffer();
        u8g2.drawStr(0, 15, "FATAL ERROR:");
        u8g2.drawStr(0, 30, "SD Card Failed!");
        u8g2.sendBuffer();
        Serial.println("FATAL ERROR: SD Card initialization failed! Check wiring.");
        while (1) delay(1000); // Program halts if SD fails
    }
    Serial.println("SD Card initialized.");
    
    // 6. Load first song
    Serial.println("6. Loading initial song file...");
    if (!openNextSong()) {
        u8g2.clearBuffer();
        u8g2.drawStr(0, 15, "FATAL ERROR:");
        u8g2.drawStr(0, 30, "File Not Found!");
        u8g2.sendBuffer();
        Serial.println("FATAL ERROR: Failed to open initial song file. Check file names.");
        while (1) delay(1000); // Program halts if initial file load fails
    }
    Serial.println("Setup Complete. Entering loop...");
}

void loop() {
    // Perform analysis and update haptics/display periodically
    if (millis() - lastAnalysisTime >= ANALYSIS_INTERVAL) {
        lastAnalysisTime = millis();
        analyzeAndDisplayAudio();
    }
}

