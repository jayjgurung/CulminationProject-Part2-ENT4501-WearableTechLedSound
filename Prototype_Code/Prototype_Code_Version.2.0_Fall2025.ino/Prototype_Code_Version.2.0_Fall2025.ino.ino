#include <IRremote.h>
#include <FastLED.h>

// ===================== LED STRIP CONFIG =====================
#define NUM_LEDS 50          // Number of addressable LEDs
#define DATA_PIN 8           // Addressable LED data pin
#define LED_TYPE WS2812      // LED chipset
#define COLOR_ORDER GRB
CRGB leds[NUM_LEDS];

// ===== MCU-AWARE PIN MAP (ESP32-WROVER-IE safe header pins) =====
#if defined(ARDUINO_ARCH_ESP32)
  // Board in your photo lacks a GPIO5 header; use GPIO23 for WS2812 data.
  #define PIN_DATA 23   // WS2812 DIN
  #define PIN_IR   14   // IR receiver OUT (left side header labeled 14)
  #define PIN_LED1 16   // indicator LEDs
  #define PIN_LED2 17
  #define PIN_LED3 18
  #define PIN_LED4 19
  #define PIN_LED5 21
  #define PIN_MIC  34   // analog-only input (ADC1)
#else
  // AVR fallback (e.g., UNO)
  #define PIN_DATA 8
  #define PIN_IR   7
  #define PIN_LED1 2
  #define PIN_LED2 3
  #define PIN_LED3 4
  #define PIN_LED4 5
  #define PIN_LED5 6
  #define PIN_MIC  A0
#endif

// Bind the above to the names used by the rest of the sketch
#undef DATA_PIN
#define DATA_PIN PIN_DATA

const uint8_t IR_PIN   = PIN_IR;
const uint8_t LED1_PIN = PIN_LED1;
const uint8_t LED2_PIN = PIN_LED2;
const uint8_t LED3_PIN = PIN_LED3;
const uint8_t LED4_PIN = PIN_LED4;
const uint8_t LED5_PIN = PIN_LED5;
const uint8_t MIC_PIN  = PIN_MIC;

// ===================== AUDIO / MIC CONFIG =====================
// const uint8_t MIC_PIN = A0;          // Analog mic input
const uint16_t ADC_MAX = 1023;       // 10-bit ADC

// Adaptive baseline & envelope follower
// (two-pole EMA: slow for baseline (dc), faster for envelope)
float micBaseline = 512.0f;          // Start near mid-rail for electret/INMP modules
float micEMA = 512.0f;               // Fast envelope follower
const float BASELINE_ALPHA = 0.001f; // Super slow drift to track room noise
const float ENV_ALPHA      = 0.10f;  // Fast response to transients

// Thresholds derived from envelope (scaled 0..1 later)
uint16_t micThresholdRaw = 40;       // Minimum deviation to trigger (raw ADC units)

// Brightness dynamics
uint8_t globalBrightness = 128;      // Default brightness

// ===================== IR REMOTE CONFIG =====================
// const uint8_t IR_PIN = 7;            // IR receiver pin

// Known raw codes from the user's remote (NEC 32-bit, LSB first as provided previously)
// Power OFF
#define IR_POWER_OFF 0xE916FF00
// 1..6
#define IR_BTN_1 0xF30CFF00
#define IR_BTN_2 0xE718FF00
#define IR_BTN_3 0xA15EFF00
#define IR_BTN_4 0xF708FF00
#define IR_BTN_5 0xE31CFF00
#define IR_BTN_6 0xA55AFF00

// ===================== TIMING =====================
// Millis-based frame pacing to keep animations smooth and IR responsive
const uint16_t FPS = 90;                   // Target frames per second for LED updates
const uint32_t FRAME_DT = 1000UL / FPS;    // ms per frame
uint32_t lastFrameMs = 0;

// FX1 (noise fields) time scaling
#define TIME_FACTOR_HUE 60
#define TIME_FACTOR_SAT 100
#define TIME_FACTOR_VAL 100

// FX2 chase params
const uint8_t FX2_SPAN = 10;         // number of lit pixels in the train
uint16_t fx2Index = 0;               // moving head index

// FX3 gating delay
uint32_t fx3LastUpdate = 0;

// ===================== APP STATE =====================

enum Mode : uint8_t {
  MODE_IDLE = 0,
  MODE_FX1,
  MODE_FX2,
  MODE_FX3,
  MODE_MIC
};

volatile Mode mode = MODE_IDLE;        // Current mode
uint32_t lastIrCode = 0;               // Last non-zero IR code (to suppress repeats)

// ===================== FORWARD DECLARATIONS =====================
void handleIR();
void setMode(Mode m);
void clearStrip();
void showIndicators();
void readMicAndUpdateEnvelope();
void runFX1_Noise();
void runFX2_Chase();
void runFX3_RandomGated();
void runMIC_ThresholdBlink();

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);

  // --- Addressable LEDs ---
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(globalBrightness);
  clearStrip();
  FastLED.show();

  // --- Indicators ---
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(LED3_PIN, OUTPUT);
  pinMode(LED4_PIN, OUTPUT);
  pinMode(LED5_PIN, OUTPUT);
  showIndicators();

  // --- IR ---
  IrReceiver.begin(IR_PIN, ENABLE_LED_FEEDBACK);

  // --- Mic ---
#if defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(10);                 // 0..1023
  analogSetPinAttenuation(MIC_PIN, ADC_11db); // ~0..3.3V range
#endif
  analogRead(MIC_PIN); // warm-up dummy read

  delay(50);
}

// ===================== LOOP =====================
void loop() {
  handleIR(); // Always keep IR responsive

  // Pace the animation to a fixed FPS
  const uint32_t now = millis();
  if (now - lastFrameMs < FRAME_DT) return;
  lastFrameMs = now;

  // Update mic envelope every frame (low cost)
  readMicAndUpdateEnvelope();

  switch (mode) {
    case MODE_FX1: runFX1_Noise(); break;
    case MODE_FX2: runFX2_Chase(); break;
    case MODE_FX3: runFX3_RandomGated(); break;
    case MODE_MIC: runMIC_ThresholdBlink(); break;
    case MODE_IDLE:
    default:
      // Low-power idle: subtle breathing on first LED to show system is alive
      {
        uint8_t b = beatsin8(8, 8, 32); // slow subtle pulse 8-32
        leds[0] = CHSV(160, 200, b);
        for (int i = 1; i < NUM_LEDS; ++i) leds[i] = CRGB::Black;
        FastLED.show();
      }
      break;
  }
}

// ===================== IR HANDLER =====================
void handleIR() {
  if (!IrReceiver.decode()) return;
  const uint32_t code = IrReceiver.decodedIRData.decodedRawData;
  IrReceiver.resume();

  if (code == 0) return; // ignore spurious zeros
  if (code == lastIrCode) return; // simple de-bounce for repeats
  lastIrCode = code;

  Serial.print("IR: 0x"); Serial.println(code, HEX);

  if (code == IR_POWER_OFF) {
    setMode(MODE_IDLE);
    clearStrip();
    FastLED.show();
    return;
  }

  if (code == IR_BTN_1)      setMode(MODE_FX1);
  else if (code == IR_BTN_2) setMode(MODE_FX2);
  else if (code == IR_BTN_3) setMode(MODE_FX3);
  else if (code == IR_BTN_4) setMode(MODE_IDLE);
  else if (code == IR_BTN_5) { // brightness toggle 50% / 100%
    globalBrightness = (globalBrightness < 200) ? 255 : 128;
    FastLED.setBrightness(globalBrightness);
  }
  else if (code == IR_BTN_6) setMode(MODE_MIC);
}

void setMode(Mode m) {
  mode = m;
  showIndicators();
  // When switching modes, clear or reset per-FX state
  if (mode == MODE_FX2) fx2Index = 0;
  if (mode == MODE_FX3) fx3LastUpdate = millis();
}

void showIndicators() {
  digitalWrite(LED1_PIN, mode == MODE_FX1);
  digitalWrite(LED2_PIN, mode == MODE_FX2);
  digitalWrite(LED3_PIN, mode == MODE_FX3);
  digitalWrite(LED4_PIN, mode == MODE_IDLE);
  digitalWrite(LED5_PIN, mode == MODE_MIC);
}

// ===================== MIC / ENVELOPE =====================
void readMicAndUpdateEnvelope() {
  const int raw = analogRead(MIC_PIN); // 0..1023

  // Slow baseline tracks room DC/noise very slowly
  micBaseline = (1.0f - BASELINE_ALPHA) * micBaseline + BASELINE_ALPHA * raw;

  // Deviation from baseline
  float deviation = fabsf(raw - micBaseline);

  // Fast envelope follows transients
  micEMA = (1.0f - ENV_ALPHA) * micEMA + ENV_ALPHA * (micBaseline + deviation);
}

// Helper: get a 0..255 envelope magnitude
uint8_t micLevel255() {
  float level = fabsf(micEMA - micBaseline); // envelope above baseline
  level = constrain(level, 0.0f, (float)ADC_MAX);
  return (uint8_t) map((long)level, 0, 200, 0, 255); // scale: ~200 ADC counts -> full
}

// ===================== EFFECTS =====================
// FX1: Organic noise field rainbow (non-blocking)
void runFX1_Noise() {
  const uint32_t ms = millis();
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t hue = inoise16(ms * TIME_FACTOR_HUE, i * 1000, 0) >> 8;
    uint8_t sat = inoise16(ms * TIME_FACTOR_SAT, i * 2000, 1000) >> 8;
    uint8_t val = inoise16(ms * TIME_FACTOR_VAL, i * 3000, 2000) >> 8;

    sat = map(sat, 0, 255, 30, 255);
    val = map(val, 0, 255, 64, 255);

    leds[i] = CHSV(hue, sat, val);
  }
  FastLED.show();
}

// FX2: Blue chase train (non-blocking)
void runFX2_Chase() {
  clearStrip();
  for (uint8_t j = 0; j < FX2_SPAN; j++) {
    int idx = (fx2Index + j) % NUM_LEDS;
    leds[idx] = CRGB(0, 0, 255);
  }
  fx2Index = (fx2Index + 1) % NUM_LEDS;
  FastLED.show();
}

// FX3: Random colors gated by audio level (non-blocking)
void runFX3_RandomGated() {
  const uint8_t level = micLevel255();
  if (level > 32) { // above gate -> paint randoms
    for (int i = 0; i < NUM_LEDS; i++) {
      leds[i].setRGB(random8(), random8(), random8());
    }
  } else {
    // below gate -> decay to black
    for (int i = 0; i < NUM_LEDS; i++) leds[i].nscale8_video(220);
  }
  FastLED.show();
}

// MIC MODE: Discrete LEDs blink with threshold, strip reacts to level
void runMIC_ThresholdBlink() {
  const uint8_t level = micLevel255();

  // Discrete LED bargraph on LED1..LED5 as simple feedback
  digitalWrite(LED1_PIN, level > 10);
  digitalWrite(LED2_PIN, level > 40);
  digitalWrite(LED3_PIN, level > 80);
  digitalWrite(LED4_PIN, level > 120);
  digitalWrite(LED5_PIN, level > 180);

  // Strip brightness and hue respond to level
  uint8_t hue = map(level, 0, 255, 160, 0); // blue -> red as it gets louder
  uint8_t val = map(level, 0, 255, 16, 255);
  fill_solid(leds, NUM_LEDS, CHSV(hue, 255, val));
  FastLED.show();
}

// ===================== UTIL =====================
void clearStrip() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
}