/*
  ESP32-WROVER-IE  •  I2S Mic + 5x WS2812 LEDs + IR Remote
  ---- Upgrades -------------------------------------------
  - Ambient heartbeat = RED only, bigger dynamic swing
  - Audio-reactive = AGC + gamma + faster smoothing for punchier response
  - Your "1" key = 0x0C (already set)
*/

#include <Arduino.h>
#include <driver/i2s.h>
#include <arduinoFFT.h>
#include <FastLED.h>
#include <IRremote.hpp>

/* ========================= USER SETTINGS ========================= */

// --- Pins (keep as you wired) ---
#define LED_DATA_PIN   23
#define IR_RECV_PIN    34
#define I2S_WS_PIN     25
#define I2S_SCK_PIN    26
#define I2S_SD_PIN     33

// --- LEDs ---
#define NUM_LEDS       5
#define LED_TYPE       WS2812B
#define LED_COLOR_ORDER RGB
#define LED_BRIGHTNESS_LIMIT  220   // ↑ a bit for more punch

// --- Audio & FFT ---
const uint16_t FFT_SIZE    = 1024;
const uint32_t SAMPLE_RATE = 16000;
const float NOISE_FLOOR_DB = -72.0f;

// ↓ Faster reaction (was 0.65)
const float SMOOTH_ALPHA   = 0.50f;

// --- Frequency Bands (Hz) ---
struct Band { float fLow; float fHigh; };
Band bands[NUM_LEDS] = {
  {  20,   60  },  // Sub
  {  60,  250  },  // Bass
  { 250,  500  },  // Low-Mid
  { 500, 2000  },  // Mid
  {2000, 8000  }   // Treble
};

// --- Ambient heartbeat (RED only now) ---
const uint8_t  AMBIENT_MIN     = 4;      // ↓ can get very dim
const uint8_t  AMBIENT_MAX     = 110;    // ↑ stronger peak
const uint16_t AMBIENT_MIN_MS  = 700;
const uint16_t AMBIENT_MAX_MS  = 1300;

// --- IR control ---
const uint8_t EXPECTED_PROTOCOL = NEC;   // hint only
const uint8_t IR_KEY_1_FALLBACK = 0x0C;  // your “1” key

/* ========================= GLOBALS ========================= */

CRGB leds[NUM_LEDS];

static float vReal[FFT_SIZE];
static float vImag[FFT_SIZE];
ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

// Smoothed band values (0..1)
float bandSmooth[NUM_LEDS] = {0};

// Simple automatic gain control (AGC)
float agcGain      = 1.0f;
const float AGC_ALPHA     = 0.1f;   // how fast AGC adapts
const float AGC_TARGET    = 0.30f;   // desired average band level
const float AGC_MIN_GAIN  = 0.5f;
const float AGC_MAX_GAIN  = 8.0f;

// Gamma to lift quiet details without blowing peaks
const float DISPLAY_GAMMA = 0.60f;   // <1.0 = more sensitive at low levels

enum Mode { MODE_AMBIENT = 0, MODE_AUDIO = 1 };
volatile Mode runMode = MODE_AMBIENT;

uint32_t ambientStartMs = 0;
uint16_t ambientPeriod  = 1000;

uint8_t learnedKey1 = IR_KEY_1_FALLBACK;

/* ========================= UTILITIES ========================= */

int freqToBin(float f) {
  int bin = (int)roundf((f * FFT_SIZE) / SAMPLE_RATE);
  return constrain(bin, 1, (int)FFT_SIZE/2 - 1);
}

float magToDb(float mag) {
  float db = 20.0f * log10f(max(mag, 1e-6f));
  if (isnan(db) || isinf(db)) db = NOISE_FLOOR_DB;
  return max(db, NOISE_FLOOR_DB);
}

float smoothExp(float prev, float cur, float alpha) {
  return alpha * prev + (1.0f - alpha) * cur;
}

// Fixed band colors (blue → red)
CRGB colorForIndex(int i) {
  const CRGB palette[NUM_LEDS] = {
    CRGB(  0,  0,255), // Sub = blue
    CRGB(  0,255,255), // Bass = cyan
    CRGB(  0,255,  0), // Low-mid = green
    CRGB(255,255,  0), // Mid = yellow
    CRGB(255,  0,  0)  // Treble = red
  };
  return palette[constrain(i, 0, NUM_LEDS-1)];
}

/* ========================= I2S SETUP/READ ========================= */

void setupI2SMic() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD_PIN
  };

  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

size_t readI2SBlock(int16_t *dest, size_t samplesWanted) {
  size_t bytesRead = 0;
  size_t neededBytes = samplesWanted * sizeof(int32_t);
  static int32_t i2sRaw[FFT_SIZE];

  i2s_read(I2S_NUM_0, (void*)i2sRaw, neededBytes, &bytesRead, portMAX_DELAY);
  size_t frames = bytesRead / sizeof(int32_t);

  for (size_t i = 0; i < frames; i++) {
    dest[i] = (int16_t)(i2sRaw[i] >> 11);
  }
  return frames;
}

/* ========================= IR SETUP/HANDLER ========================= */

void setupIR() {
  IrReceiver.begin(IR_RECV_PIN, ENABLE_LED_FEEDBACK);
  Serial.println(F("IR ready. Press '1' to switch to audio-reactive."));
}

void handleIR() {
  if (IrReceiver.decode()) {
    auto &d = IrReceiver.decodedIRData;

    // Uncomment to see what you get:
    // Serial.print(F("IR: proto=")); Serial.print(d.protocol);
    // Serial.print(F(" addr=0x")); Serial.print(d.address, HEX);
    // Serial.print(F(" cmd=0x"));  Serial.println(d.command, HEX);

    if (d.command == learnedKey1 || d.command == IR_KEY_1_FALLBACK) {
      runMode = MODE_AUDIO;
      // brief visual confirm: flash all LEDs white quickly
      fill_solid(leds, NUM_LEDS, CRGB::White); FastLED.show(); delay(60);
      fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show();
      Serial.println(F("Mode: AUDIO-REACTIVE"));
    }
    IrReceiver.resume();
  }
}

/* ========================= AMBIENT MODE ========================= */

void resetAmbient() {
  ambientStartMs = millis();
  ambientPeriod  = random(AMBIENT_MIN_MS, AMBIENT_MAX_MS + 1);
}

void renderAmbient() {
  // Heartbeat-like BRIGHT red breathing with a randomized period each loop
  uint32_t t = millis() - ambientStartMs;
  if (t > ambientPeriod) { resetAmbient(); t = 0; }

  // Cosine 0..1
  float phase = (2.0f * PI * t) / ambientPeriod;
  float wave  = 0.5f * (1.0f - cosf(phase));

  // Slight “double-beat” accent: sharpen peak using pow()
  float shaped = powf(wave, 0.7f);  // <1.0 lifts midrange, punchier

  uint8_t b = AMBIENT_MIN + (uint8_t)((AMBIENT_MAX - AMBIENT_MIN) * shaped);

  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Red;           // RED only as requested
    leds[i].nscale8_video(b);
  }
  FastLED.show();
}

/* ========================= AUDIO MODE ========================= */

void computeFFTAndBands() {
  static int16_t pcm[FFT_SIZE];
  size_t got = readI2SBlock(pcm, FFT_SIZE);
  if (got < FFT_SIZE) for (size_t i = got; i < FFT_SIZE; i++) pcm[i] = 0;

  // Remove DC + window
  double mean = 0;
  for (int i = 0; i < FFT_SIZE; i++) mean += pcm[i];
  mean /= FFT_SIZE;

  for (int i = 0; i < FFT_SIZE; i++) {
    float w = 0.5f * (1.0f - cosf((2.0f * PI * i) / (FFT_SIZE - 1)));
    vReal[i] = ((float)pcm[i] - (float)mean) * w;
    vImag[i] = 0.0f;
  }

  FFT.windowing(FFT_WIN_TYP_HANN, FFT_FORWARD); // safe duplicate
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();

  // Per-band raw levels (0..1 before smoothing), and average for AGC
  float avgRaw = 0.0f;

  for (int b = 0; b < NUM_LEDS; b++) {
    int binL = freqToBin(bands[b].fLow);
    int binH = freqToBin(bands[b].fHigh);
    if (binH <= binL) binH = binL + 1;

    double sum = 0;
    for (int k = binL; k <= binH && k < FFT_SIZE/2; k++) sum += vReal[k];

    float db   = magToDb((float)sum);
    float norm = (db - NOISE_FLOOR_DB) / (0.0f - NOISE_FLOOR_DB);
    norm = constrain(norm, 0.0f, 1.0f);

    avgRaw += norm;
    // Apply AGC gain before smoothing (makes small signals visible)
    float boosted = constrain(norm * agcGain, 0.0f, 1.0f);

    bandSmooth[b] = smoothExp(bandSmooth[b], boosted, SMOOTH_ALPHA);
  }

  avgRaw /= NUM_LEDS;

  // Update AGC toward target level
  if (!isnan(avgRaw) && avgRaw > 0.0001f) {
    float corr = AGC_TARGET / avgRaw;
    agcGain = agcGain * (1.0f - AGC_ALPHA) + corr * AGC_ALPHA;
    agcGain = constrain(agcGain, AGC_MIN_GAIN, AGC_MAX_GAIN);
  }
}

void renderAudioReactive() {
  computeFFTAndBands();

  for (int i = 0; i < NUM_LEDS; i++) {
    // Gamma shaping for wider perceived dynamic range
    float shaped = powf(constrain(bandSmooth[i], 0.0f, 1.0f), DISPLAY_GAMMA);

    uint8_t lvl = (uint8_t)(shaped * 255.0f);
    lvl = min<uint8_t>(lvl, LED_BRIGHTNESS_LIMIT);

    leds[i] = colorForIndex(i);
    leds[i].nscale8_video(max<uint8_t>(lvl, 6));
  }
  FastLED.show();
}

/* ========================= SETUP/LOOP ========================= */

void setup() {
  Serial.begin(115200);
  delay(100);

  FastLED.addLeds<LED_TYPE, LED_DATA_PIN, LED_COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS_LIMIT);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  setupIR();
  setupI2SMic();

  resetAmbient();
  Serial.println(F("Boot: Ambient RED heartbeat. Press '1' for audio-reactive."));
}

void loop() {
  handleIR();
  if (runMode == MODE_AMBIENT) renderAmbient();
  else                         renderAudioReactive();
}