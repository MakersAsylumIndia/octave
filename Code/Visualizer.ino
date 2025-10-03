#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <arduinoFFT.h>
#include <Wire.h>
#include <U8g2lib.h> // For SH1106 OLED display

// --- HARDWARE CONFIGURATION ---
// Change this if your SD card's Chip Select pin is different
#define SD_CS_PIN 5           // Chip Select pin for SD card (ESP32 typically uses 5 or 15)

// Using hardware I2C for SH1106 128x64 display (No Multiplexer required here)
// Address is often 0x3C or 0x3D
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// --- FFT & AUDIO CONFIGURATION ---
#define SAMPLES 1024          // Power of 2 (FFT size)
#define SAMPLING_FREQ 16000   // WAV sampling rate (must match your file's sample rate)
#define WAV_HEADER_SIZE 44    // Standard WAV header size in bytes
#define ANALYSIS_INTERVAL 200 // Analyze every 200 milliseconds for smooth visuals

// Frequency Resolution (dF) = 15.625 Hz/bin
// We will only visualize the first 64 bins (up to 1000 Hz) for better resolution on the 128x64 screen.
#define VISUAL_BINS 64
#define LOW_FREQ_END_HZ    200   // For display label
#define MID_FREQ_END_HZ    2000  // For display label

// --- SCALING CONSTANT ---
// This value is used to normalize the raw FFT magnitude (vReal[k]) into a 0-255 range.
// TUNE THIS if your PWM values are always 0 or always 255.
// We increased this from 20000.0 to 50000.0 to prevent constant clipping at 255.
#define MAX_BIN_MAGNITUDE 300000

// FFT arrays and object (must explicitly use <double>)
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT; 

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
  
  for (uint8_t i = 0; i < numBins; i++) {
    // Normalize magnitude for visual display (logarithmic scaling is often best for FFT)
    // We use a simple multiplier and constrain to fit the screen height.
    double magnitude = magnitudes[i + 1]; // Skip bin 0 (DC component)
    int height = constrain(log10(magnitude) * 12, 1, maxBarHeight); 
    
    // Draw the bar (i*2 gives a 2-pixel width per bar, 128 / 64 = 2)
    // Bars are drawn from the bottom up (y=64)
    u8g2.drawBox(i * 2, u8g2.getHeight() - height, 2, height); 
  }

  // Draw frequency boundary lines for reference
  u8g2.drawVLine(LOW_FREQ_END_HZ / (SAMPLING_FREQ / SAMPLES) * 2, u8g2.getHeight() - 5, 5); // Line for 200Hz
  u8g2.drawVLine(MID_FREQ_END_HZ / (SAMPLING_FREQ / SAMPLES) * 2, u8g2.getHeight() - 5, 5); // Line for 2000Hz (off screen)
  
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

    // --- 3. Conditional Debug Output: Print scaled PWM values (0-255) for the first 10 bins ---
    if (millis() - lastSerialPrintTime >= SERIAL_PRINT_INTERVAL) {
        lastSerialPrintTime = millis();
        
        Serial.println("--- Scaled Bin PWM Values (First 10) (Printed Every 15s) ---");
        // We print bins 1 through 10 (bin 0 is the DC component and usually ignored)
        for (int k = 1; k <= 10; k++) {
            // Frequency (F) = Bin (k) * Resolution (dF)
            double freq = k * (SAMPLING_FREQ / (double)SAMPLES);
            
            // Scale the raw magnitude (vReal[k]) to a 0-255 range
            int pwmValue = constrain(
              map((long)vReal[k], 0, (long)MAX_BIN_MAGNITUDE, 0, 255), 
              0, 
              255
            );
            
            Serial.print("Bin ");
            Serial.print(k);
            Serial.print(" (");
            Serial.print(freq, 1); // 1 decimal place for frequency
            Serial.print(" Hz): PWM=");
            Serial.println(pwmValue); 
        }
    }

    // --- 4. Display Visualization ---
    drawAnalyzer(vReal, VISUAL_BINS); 
}

// --- ARDUINO SETUP AND LOOP ---
void setup() {
    // Serial initialization for debugging
    Serial.begin(9600); 
    while (!Serial && millis() < 3000) { delay(10); }
    delay(500);         
    Serial.println("--- Starting Audio Visualizer ---");
    
    // 1. OLED Initialization
    Serial.println("1. Initializing U8g2 OLED...");
    Wire.begin(); // Start I2C bus for the display
    u8g2.begin();
    u8g2.setFont(u8g2_font_ncenB08_tr); 
    u8g2.clearBuffer();
    u8g2.drawStr(0, 15, "Starting Up...");
    u8g2.drawStr(0, 30, "Check SD Card!");
    u8g2.sendBuffer();
    Serial.println("OLED initialized.");

    // 2. SD Card Initialization
    Serial.println("2. Initializing SD Card...");
    if (!SD.begin(SD_CS_PIN)) {
        u8g2.clearBuffer();
        u8g2.drawStr(0, 15, "FATAL ERROR:");
        u8g2.drawStr(0, 30, "SD Card Failed!");
        u8g2.sendBuffer();
        Serial.println("FATAL ERROR: SD Card initialization failed! Check wiring.");
        while (1) delay(1000); // Program halts if SD fails
    }
    Serial.println("SD Card initialized.");
    
    // 3. Load first song
    Serial.println("3. Loading initial song file...");
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
    // Perform analysis and update display periodically
    if (millis() - lastAnalysisTime >= ANALYSIS_INTERVAL) {
        lastAnalysisTime = millis();
        analyzeAndDisplayAudio();
    }
}

