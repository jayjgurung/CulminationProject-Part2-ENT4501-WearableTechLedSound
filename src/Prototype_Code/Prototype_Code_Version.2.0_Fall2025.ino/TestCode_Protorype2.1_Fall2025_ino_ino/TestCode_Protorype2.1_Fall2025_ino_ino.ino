/*
  ESP32-WROVER-IE  •  I2S Mic + 5x WS2812 LEDs + IR Remote
  ---------------------------------------------------------
  Modes:
    - Default: Ambient heartbeat glow (soft randomized pulse)
    - IR "1" : Audio-reactive; 5 LEDs = 5 frequency bands (sub→treble)
               Brighter = louder in that band; colors: blue→red across strip

  Wiring (change pins below to match your setup):
    I2S MIC (e.g., INMP441)
      L/R CLK (WS) -> I2S_WS_PIN
      BCLK       -> I2S_SCK_PIN
      DOUT       -> I2S_SD_PIN
      3V3, GND to ESP32 3V3/GND
      Note: INMP441 SD Mode: connect L/R SEL to GND (left) or 3V3 (right)

    LEDs (WS2812B)
      DIN -> LED_DATA_PIN (through 330Ω resistor recommended)
      5V  -> external 5V supply
      GND -> common GND with ESP32

    IR Receiver (e.g., VS1838/TSOP38238)
      OUT -> IR_RECV_PIN
      VCC -> 3V3 (check your module; many support 3.3–5V)
      GND -> GND

  Power Tip:
      Even 5 LEDs can draw ~300mA at full white. Power the LEDs from a stable 5V
      supply and always tie grounds together.
*/

#include <Arduino.h>
#include <driver/i2s.h>     // ESP32 I2S driver
#include <arduinoFFT.h>     // v1.6+ templated API
#include <FastLED.h>        // Addressable LEDs
#include <IRremote.hpp>     // v4+ API

/* ========================= USER SETTINGS ========================= */

// -------- Pins (EDIT to match your wiring) --------
#define LED_DATA_PIN   23       // WS2812 data pin
#define IR_RECV_PIN    34      // IR receiver output pin
#define I2S_WS_PIN     25      // LRCLK / WS
#define I2S_SCK_PIN    26      // BCLK / SCK
#define I2S_SD_PIN     33      // DOUT from mic -> ESP32

// -------- LEDs --------
#define NUM_LEDS       5
#define LED_TYPE       WS2812B
#define LED_COLOR_ORDER GRB
#define LED_BRIGHTNESS_LIMIT  200  // cap global brightness (0..255)

// -------- Audio & FFT --------
const uint16_t FFT_SIZE    = 1024;   // power of 2
const uint32_t SAMPLE_RATE = 16000;  // Nyquist 8 kHz
const float NOISE_FLOOR_DB = -72.0f; // clamp floor to tame silence
const float SMOOTH_ALPHA   = 0.65f;  // 0..1 (higher = smoother, slower)

// -------- Frequency Bands (Hz) for 5 LEDs --------
//   LED 0: Sub (20–60)
//   LED 1: Bass (60–250)
//   LED 2: Low Mid (250–500)
//   LED 3: Mid (500–2000)
//   LED 4: Treble (2000–8000)
// NOTE: With 16 kHz sampling, max usable ~8 kHz.
struct Band { float fLow; float fHigh; };
Band bands[NUM_LEDS] = {
  { 20,   60  },
  { 60,   250 },
  { 250,  500 },
  { 500,  2000},
  { 2000, 8000}
};

// -------- Ambient heartbeat look --------
const uint8_t AMBIENT_MIN = 6;    // min brightness for glow
const uint8_t AMBIENT_MAX = 32;   // max brightness for glow
const uint16_t AMBIENT_MIN_MS = 800;   // min beat period
const uint16_t AMBIENT_MAX_MS = 1400;  // max beat period

// -------- IR control --------
// Many NEC remotes send 0x45 for the "1" key, but yours may differ.
// If "1" doesn’t switch modes, open Serial Monitor once and read the code printed.
const uint8_t EXPECTED_PROTOCOL = NEC;   // hint for readability
const uint8_t IR_KEY_1_FALLBACK = 0x0C;  // change to your remote’s code if needed

/* ========================= GLOBALS ========================= */

// LED buffer
CRGB leds[NUM_LEDS];

// I2S / FFT buffers (float for arduinoFFT v1.6+ template)
static float vReal[FFT_SIZE];
static float vImag[FFT_SIZE];

// FFT instance bound to our buffers (templated API)
ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

// Smoothed band values (0..1)
float bandSmooth[NUM_LEDS] = {0};

// Running state
enum Mode { MODE_AMBIENT = 0, MODE_AUDIO = 1 };
volatile Mode runMode = MODE_AMBIENT;

// Ambient timing
uint32_t ambientStartMs = 0;
uint16_t ambientPeriod  = 1000;

// IR command capture (optional)
uint8_t learnedKey1 = IR_KEY_1_FALLBACK;

/* ========================= UTILITIES ========================= */

// Map frequency in Hz to FFT bin index
int freqToBin(float freqHz) {
  // Bin width ≈ SAMPLE_RATE / FFT_SIZE
  int bin = (int)roundf((freqHz * FFT_SIZE) / SAMPLE_RATE);
  bin = constrain(bin, 1, (int)FFT_SIZE/2 - 1);
  return bin;
}

// Simple dBFS conversion with floor clamp
float magToDb(float mag) {
  float db = 20.0f * log10f(max(mag, 1e-6f));
  if (isnan(db) || isinf(db)) db = NOISE_FLOOR_DB;
  return max(db, NOISE_FLOOR_DB);
}

// Exponential smoothing
float smoothExp(float prev, float cur, float alpha) {
  return alpha * prev + (1.0f - alpha) * cur;
}

// Palette: 5 colors from blue → red (you can customize)
CRGB colorForIndex(int i) {
  // Predefined gradients: blue, cyan, green, yellow, red
  const CRGB palette[NUM_LEDS] = {
    CRGB(  0,  0,255), // blue
    CRGB(  0,255,255), // cyan
    CRGB(  0,255,  0), // green
    CRGB(255,255,  0), // yellow
    CRGB(255,  0,  0)  // red
  };
  return palette[constrain(i, 0, NUM_LEDS-1)];
}

/* ========================= I2S SETUP/READ ========================= */

void setupI2SMic() {
  // Configure I2S in RX (receive) mode for PDM/I2S mics like INMP441 (I2S format, 24-bit typical)
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // read 32-bit frames, we’ll downscale
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // use LEFT channel (set L/R on mic board)
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK_PIN,
    .ws_io_num  = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD_PIN
  };

  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

size_t readI2SBlock(int16_t *dest, size_t samplesWanted) {
  // The mic delivers 24-bit (often packed into 32-bit). We read int32_t frames and shift to 16-bit.
  size_t bytesRead = 0;
  size_t neededBytes = samplesWanted * sizeof(int32_t);
  static int32_t i2sRaw[FFT_SIZE]; // temp buffer

  i2s_read(I2S_NUM_0, (void*)i2sRaw, neededBytes, &bytesRead, portMAX_DELAY);
  size_t frames = bytesRead / sizeof(int32_t);

  for (size_t i = 0; i < frames; i++) {
    // Shift right to approximately 16-bit range; adjust shift based on your mic’s amplitude
    dest[i] = (int16_t)(i2sRaw[i] >> 11); // ~21-bit → 10-bit headroom
  }
  return frames;
}

/* ========================= IR SETUP/HANDLER ========================= */

void setupIR() {
  IrReceiver.begin(IR_RECV_PIN, ENABLE_LED_FEEDBACK); // shows feedback on onboard LED (if present)
  // Optional: Serial prints help you discover your remote’s codes
  Serial.println(F("IR ready. Press keys to see codes; set IR_KEY_1_FALLBACK accordingly if needed."));
}

void handleIR() {
  if (IrReceiver.decode()) {
    auto &d = IrReceiver.decodedIRData;
    // Print once to learn your remote’s "1" code if fallback doesn’t work
    Serial.print(F("IR: proto=")); Serial.print(d.protocol);
    Serial.print(F(" addr=0x")); Serial.print(d.address, HEX);
    Serial.print(F(" cmd=0x"));  Serial.println(d.command, HEX);

    // Learn/update key on first valid decode (optional)
    if (d.command != 0x00 && d.command != 0xFF) {
      // you could persist this in NVS if desired
      // learnedKey1 = d.command; // uncomment to auto-learn the last pressed key
    }

    // Toggle to AUDIO mode when "1" is pressed (by command code)
    if (d.command == learnedKey1 || d.command == IR_KEY_1_FALLBACK) {
      runMode = MODE_AUDIO;
      Serial.println(F("Mode: AUDIO-REACTIVE"));
    }

    IrReceiver.resume(); // ready for next
  }
}

/* ========================= AMBIENT MODE ========================= */

void resetAmbient() {
  ambientStartMs = millis();
  ambientPeriod  = random(AMBIENT_MIN_MS, AMBIENT_MAX_MS + 1);
}

void renderAmbient() {
  // Heartbeat-like breathing: brightness follows a smooth cosine curve,
  // with a randomized period each time we reset.
  uint32_t t = millis() - ambientStartMs;
  if (t > ambientPeriod) {
    resetAmbient();
    t = 0;
  }
  float phase = (2.0f * PI * t) / ambientPeriod;
  // Cosine curve 0..1
  float wave = 0.5f * (1.0f - cosf(phase));
  uint8_t b = AMBIENT_MIN + (uint8_t)((AMBIENT_MAX - AMBIENT_MIN) * wave);

  // Slight random flicker per LED to feel organic
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t jitter = random(0, 6); // small variability
    CRGB c = colorForIndex(i);
    leds[i] = c;
    leds[i].nscale8_video(constrain(b + jitter, AMBIENT_MIN, AMBIENT_MAX));
  }

  FastLED.show();
}

/* ========================= AUDIO MODE ========================= */

void computeFFTAndBands() {
  // 1) Capture time-domain audio
  static int16_t pcm[FFT_SIZE];
  size_t got = readI2SBlock(pcm, FFT_SIZE);
  if (got < FFT_SIZE) {
    // If DMA didn’t fill, pad remaining with zeros
    for (size_t i = got; i < FFT_SIZE; i++) pcm[i] = 0;
  }

  // 2) Convert to float, remove DC, apply window
  // Compute mean (DC)
  double mean = 0;
  for (int i = 0; i < FFT_SIZE; i++) mean += pcm[i];
  mean /= FFT_SIZE;

  // Hann window + DC removal
  for (int i = 0; i < FFT_SIZE; i++) {
    float w = 0.5f * (1.0f - cosf((2.0f * PI * i) / (FFT_SIZE - 1)));
    float centered = (float)pcm[i] - (float)mean;
    vReal[i] = centered * w;
    vImag[i] = 0.0f;
  }

  // 3) FFT
  FFT.windowing(FFT_WIN_TYP_HANN, FFT_FORWARD); // already applied, but harmless to keep consistent
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();

  // 4) Derive band energies in dBFS-like scale 0..1
  // Integrate magnitudes across each band's bin range
  for (int b = 0; b < NUM_LEDS; b++) {
    int binL = freqToBin(bands[b].fLow);
    int binH = freqToBin(bands[b].fHigh);
    if (binH <= binL) binH = binL + 1;

    // Sum magnitudes in that region
    double sum = 0;
    for (int k = binL; k <= binH && k < FFT_SIZE/2; k++) {
      sum += vReal[k];
    }
    // Convert to dB-ish
    float db = magToDb((float)sum);

    // Normalize dB range to 0..1 (NOISE_FLOOR_DB .. 0 dB)
    float norm = (db - NOISE_FLOOR_DB) / (0.0f - NOISE_FLOOR_DB);
    norm = constrain(norm, 0.0f, 1.0f);

    // Smooth it to avoid jitter
    bandSmooth[b] = smoothExp(bandSmooth[b], norm, SMOOTH_ALPHA);
  }
}

void renderAudioReactive() {
  computeFFTAndBands();

  // Map each band to an LED’s brightness, keep its fixed color
  for (int i = 0; i < NUM_LEDS; i++) {
    CRGB base = colorForIndex(i);
    // Scale 0..1 → 0..255, and also cap by global brightness limit
    uint8_t lvl = (uint8_t)(bandSmooth[i] * 255.0f);
    lvl = min<uint8_t>(lvl, LED_BRIGHTNESS_LIMIT);

    leds[i] = base;
    leds[i].nscale8_video(max<uint8_t>(lvl, 8)); // keep a faint minimum so it doesn’t go totally black
  }

  FastLED.show();
}

/* ========================= SETUP/LOOP ========================= */

void setup() {
  Serial.begin(115200);
  delay(100);

  // LEDs
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN, LED_COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS_LIMIT);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  // IR
  setupIR();

  // I2S Mic
  setupI2SMic();

  // Start in ambient
  resetAmbient();

  Serial.println(F("Boot: Ambient heartbeat mode. Press '1' on IR remote to enable audio-reactive mode."));
}

void loop() {
  handleIR();

  if (runMode == MODE_AMBIENT) {
    renderAmbient();
  } else {
    renderAudioReactive();
  }
}