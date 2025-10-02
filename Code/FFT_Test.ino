#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <arduinoFFT.h> // Ensure you use v2.04

// --- FFT & SD CONFIGURATION ---
#define SD_CS_PIN 5           // Chip Select pin for SD card (Adjust as needed for your ESP32 board)
#define SAMPLES 1024          // Power of 2 (FFT size)
#define SAMPLING_FREQ 16000   // WAV sampling rate (adjust to match your file)
#define WAV_HEADER_SIZE 44    // Standard WAV header size in bytes

// --- MOTOR MAPPING CONFIGURATION ---
// Frequency Resolution (dF) = SAMPLING_FREQ / SAMPLES = 16000 / 1024 = 15.625 Hz/bin
#define LOW_FREQ_END_HZ    200   // e.g., Bass/Sub-bass
#define MID_FREQ_END_HZ    2000  // e.g., Vocal/Harmony
// High Freq goes up to Nyquist (8000 Hz)

// Calculate bin indices
#define LOW_FREQ_END_BIN   (uint16_t)(LOW_FREQ_END_HZ / (SAMPLING_FREQ / SAMPLES))
#define MID_FREQ_END_BIN   (uint16_t)(MID_FREQ_END_HZ / (SAMPLING_FREQ / SAMPLES))
#define MAX_MAGNITUDE      80000.0 // Tune this value based on observed peak FFT magnitudes

// --- GLOBAL VARIABLES ---
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>();

void setup() {
    Serial.begin(115200);
    while (!Serial);

    Serial.println("--- ESP32 WAV FFT Analyzer (16-bit PCM) ---");

    // --- 1. SD Card Initialization ---
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("ERROR: SD Card initialization failed!");
        return;
    }
    Serial.println("SD Card initialized.");

    // --- 2. Open WAV file ---
    File wavFile = SD.open("/livvux-creative-wave-110488.wav");
    if (!wavFile) {
        Serial.println("ERROR: WAV file '/test.wav' not found!");
        return;
    }
    Serial.println("WAV file opened.");

    // --- 3. Read Samples ---
    wavFile.seek(WAV_HEADER_SIZE); // Skip WAV header

    size_t i = 0;
    while (i < SAMPLES && wavFile.available()) {
        int16_t sample;
        
        // Read 2 bytes into a 16-bit integer (16-bit Signed PCM)
        size_t bytesRead = wavFile.read((uint8_t*)&sample, 2); 
        
        if (bytesRead == 2) {
            vReal[i] = (double)sample; // 16-bit signed PCM is already centered
            vImag[i] = 0.0;
            i++;
        } else {
             // Handle incomplete last sample or EOF
            break;
        }
    }
    wavFile.close();

    if (i < SAMPLES) {
        Serial.print("ERROR: Not enough samples in file! Read ");
        Serial.print(i);
        Serial.println(" samples.");
        return;
    }

    // --- 4. Perform FFT ---
    FFT.windowing(vReal, SAMPLES, FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.compute(vReal, vImag, SAMPLES, FFT_FORWARD);
    FFT.complexToMagnitude(vReal, vImag, SAMPLES);

    // --- 5. Find Peak Frequency ---
    double peak = 0;
    uint16_t indexOfPeak = 0;
    for (uint16_t j = 1; j < SAMPLES/2; j++) {
        if (vReal[j] > peak) {
            peak = vReal[j];
            indexOfPeak = j;
        }
    }
    double freq = (indexOfPeak * SAMPLING_FREQ) / SAMPLES;
    Serial.print("\nPeak frequency detected: "); Serial.print(freq); Serial.println(" Hz");
    Serial.print("Peak magnitude: "); Serial.println(peak);
    
    // --- 6. Map to Frequency Bands for Motor Control ---
    double lowMag = 0.0;
    double midMag = 0.0;
    double highMag = 0.0;

    for (uint16_t k = 1; k < SAMPLES/2; k++) {
        // Bin k's magnitude: vReal[k]
        
        if (k <= LOW_FREQ_END_BIN) {
            lowMag += vReal[k]; // Aggregate Low Frequencies (e.g., up to 200 Hz)
        } else if (k <= MID_FREQ_END_BIN) {
            midMag += vReal[k]; // Aggregate Mid Frequencies (e.g., 200 Hz to 2000 Hz)
        } else {
            highMag += vReal[k]; // Aggregate High Frequencies (e.g., 2000 Hz to 8000 Hz)
        }
    }

    // Normalize and Map (e.g., 0-255 for 8-bit PWM)
    int lowMotorPwm = constrain(map((long)(lowMag), 0, (long)MAX_MAGNITUDE, 0, 255), 0, 255);
    int midMotorPwm = constrain(map((long)(midMag), 0, (long)MAX_MAGNITUDE, 0, 255), 0, 180);
    int highMotorPwm = constrain(map((long)(highMag), 0, (long)MAX_MAGNITUDE, 0, 255), 0, 120);

    Serial.println("\n--- Motor Control Outputs ---");
    Serial.print("Low Freq (<"); Serial.print(LOW_FREQ_END_HZ); Serial.print("Hz) PWM: "); Serial.println(lowMotorPwm);
    Serial.print("Mid Freq (<"); Serial.print(MID_FREQ_END_HZ); Serial.print("Hz) PWM: "); Serial.println(midMotorPwm);
    Serial.print("High Freq PWM: "); Serial.println(highMotorPwm);

    // Placeholder: Use ESP32's ledcWrite for actual PWM control here
    // ledcWrite(lowChannel, lowMotorPwm);
}

void loop() {
    // This example performs analysis only once in setup().
    // For continuous analysis, move the reading and FFT logic here.
}







