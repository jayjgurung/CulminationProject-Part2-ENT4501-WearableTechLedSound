#include <Arduino.h>
#include <driver/i2s.h>
#include <FastLED.h>
#include <arduinoFFT.h>

/*** PINS (match your wiring) ***/
#define I2S_WS_PIN   25   // LRCLK / WS
#define I2S_SCK_PIN  26   // BCLK / SCK
#define I2S_SD_PIN   33   // DOUT from mic -> ESP32

#define LED_DATA_PIN 23
#define NUM_LEDS     5
#define LED_TYPE     WS2812B
#define LED_ORDER    GRB
#define MAX_BRIGHT   200

/* === SENSITIVITY KNOB (B500K pot) ===
   Wire: 3V3 -> pot end, GND -> other end, Wiper -> GPIO34 (ADC only)
   This knob scales band gates (harder/easier to trigger)
   and also raises/lowers the "true off" visual threshold. */
#define POT_PIN 34

CRGB leds[NUM_LEDS];

/*** AUDIO / FFT ***/
const uint32_t SAMPLE_RATE = 16000;
const int      FFT_SIZE    = 1024;          // power of 2
const uint16_t FRAME_MS    = 20;            // ~50 FPS

/*** CALMING / CAR TUNING ***/
static const float TOP_BIN_HZ = 6000.0f;              // Ignore hissiest top
static const float WEIGHT_SLOPE_DB_PER_OCT = -3.0f;   // Pink-ish tilt

// Base post-floor gates (now higher; will be scaled live by the pot)
static const float BAND_GATE_BASE[5] = { 0.10f, 0.12f, 0.14f, 0.16f, 0.20f };

// Extra divisors per-band to tame highs if needed
static float BAND_EXTRA_DIV[5] = { 1.00f,  1.00f,  1.05f,  1.10f,  1.60f };

// Envelope time-constants (attack = fast, release = slightly slower to avoid flicker)
static float ATTACK_MS  = 20.0f;
static float RELEASE_MS = 90.0f;  // a touch slower than 70 for better idle behavior

// Internals from time-constants
static float ATTACK_A, RELEASE_A;
static inline float alpha_for_ms(float tau_ms, float dt_ms) {
  return 1.0f - expf(-dt_ms / fmaxf(tau_ms, 1.0f));
}

// Hysteresis around gate to avoid chatter
static const float HYST_DELTA = 0.03f;

// Band state (adaptive floor + envelope + on/off)
static float bandEnv[5]   = {0};
static float bandFloor[5] = {0.40f, 0.06f, 0.03f, 0.03f, 0.03f}; // seed lower so we don't start too close to idle Env
static bool  bandOn[5]    = {false,false,false,false,false};

// Bands (sub→treble). Keep lows above engine/DC rumble.
static const float edgesHz[6] = { 85, 180, 255, 1024, 2048, 15000 };

/*** I2S setup (I2S mic capture) ***/
static void setupI2S() {
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

/*** FFT buffers & object ***/
static double vReal[FFT_SIZE];
static double vImag[FFT_SIZE];
ArduinoFFT<double> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

/*** Utils ***/
static inline float clamp01(float x){ return x<0?0:(x>1?1:x); }
static inline float fastMagf(float re, float im) {
  // Cheap magnitude approximation (saves cycles vs sqrtf)
  float a = fabsf(re), b = fabsf(im);
  return (a > b) ? (a + 0.4f*b) : (b + 0.4f*a);
}
static inline float pinkWeight(float hz) {
  // Tilt spectrum ~-3 dB/oct so lows don't dominate
  if (hz < 30.0f) hz = 30.0f;
  float oct = logf(hz/30.0f)/logf(2.0f);
  float db  = WEIGHT_SLOPE_DB_PER_OCT * oct;
  return powf(10.0f, db/20.0f);
}

/*** Band bin mapping (FFT index edges for each band) ***/
static bool bandsInited=false;
static int kTop=0, kEdge[6];
static void bandsInit() {
  if (bandsInited) return;
  float binHz = (float)SAMPLE_RATE / (float)FFT_SIZE;
  kTop = (int)floorf(TOP_BIN_HZ / binHz);
  if (kTop > (FFT_SIZE/2 - 1)) kTop = FFT_SIZE/2 - 1;
  for (int i=0;i<6;++i) {
    int k = (int)floorf(edgesHz[i] / binHz);
    if (k < 1) k = 1;
    if (k > kTop) k = kTop;
    kEdge[i] = k;
  }
  bandsInited = true;
}

/*** Read one block from I2S into vReal/vImag (also remove DC) ***/
static void readI2SBlockToFFT() {
  int32_t raw[FFT_SIZE];
  size_t br=0, need=sizeof(raw), got=0;
  uint8_t* p = (uint8_t*)raw;
  while (got < need) {
    i2s_read(I2S_NUM_0, (void*)(p+got), need-got, &br, portMAX_DELAY);
    got += br;
  }
  // Convert to 16-bit-ish and subtract mean so bin 0 isn't inflated
  float mean = 0.f;
  static int16_t s16[FFT_SIZE];
  for (int i=0;i<FFT_SIZE;i++){
    s16[i] = (int16_t)(raw[i] >> 11);
    mean += s16[i];
  }
  mean /= FFT_SIZE;
  for (int i=0;i<FFT_SIZE;i++){
    vReal[i] = (double)((float)s16[i] - mean);
    vImag[i] = 0.0;
  }
}

/*** POT reading -> sensitivity and visual cutoff ***/
static inline void adcSetup() {
  // Expand ADC range so 0..3.3V ~ full scale (ESP32 specific)
  analogReadResolution(12); // 0..4095
  analogSetPinAttenuation(POT_PIN, ADC_11db); // good for ~0..3.3V
  pinMode(POT_PIN, INPUT);
}
// Return two knobs derived from pot:
//   sensMul: multiplies BAND_GATE_BASE (0.5..3.0)
//   visCut : visual off threshold (0.08..0.22)
static inline void readKnobs(float &sensMul, float &visCut) {
  // Simple 4-sample average to steady the value
  uint32_t acc = 0;
  for (int i=0;i<4;i++) acc += analogRead(POT_PIN);
  float x = (acc / 4.0f) / 4095.0f;  // 0..1
  // Map to useful ranges
  sensMul = 0.5f + 2.5f * x;         // 0.5 .. 3.0
  visCut  = 0.08f + 0.14f * x;       // 0.08 .. 0.22
}

/*** Compute 5 calmed bands (0..1) with adaptive floors, gating & envelopes ***/
static void computeBandsCalmed_fromFFT(double *re, double *im, float outBands[5], const float sensMul) {
  bandsInit();
  float sum[5] = {0};
  float binHz = (float)SAMPLE_RATE / (float)FFT_SIZE;

  // Integrate FFT bins into 5 bands with pink weighting
  for (int k=1; k<=kTop; ++k) {
    float hz = k * binHz;
    float mag = fastMagf((float)re[k], (float)im[k]);
    float v = mag * pinkWeight(hz);
    int b=0; while (b<5 && k >= kEdge[b+1]) b++; if (b>4) b=4;
    sum[b] += v;
  }

  for (int b=0;b<5;++b){
    int widthBins = max(1, kEdge[b+1] - kEdge[b]);
    float raw  = sum[b] / widthBins;

    // Normalize per-band so lows don't dominate; extra div for treble
    static const float NORM_SCALE[5] = { 0.00018f, 0.00028f, 0.00030f, 0.00030f, 0.00030f };
    float norm = raw * NORM_SCALE[b] / BAND_EXTRA_DIV[b];
    norm = clamp01(norm);

    // Adaptive floor that slowly follows the local baseline (with leak down)
    static const float FLOOR_ALPHA_UP[5] = { 0.0006f, 0.0015f, 0.0020f, 0.0020f, 0.0020f };
    static const float FLOOR_CEIL [5]    = { 0.60f,   0.55f,   0.55f,   0.55f,   0.55f   };
    static const float FLOOR_LEAK[5]     = { 0.00010f,0.00008f,0.00008f,0.00008f,0.00008f };

    bandFloor[b] = (1.0f - FLOOR_ALPHA_UP[b]) * bandFloor[b] + FLOOR_ALPHA_UP[b] * norm;
    bandFloor[b] = fminf(bandFloor[b], FLOOR_CEIL[b]);
    bandFloor[b] = fmaxf(0.0f, bandFloor[b] - FLOOR_LEAK[b]);

    // Delta above floor
    float over = norm - bandFloor[b];
    if (over < 0) over = 0;

    // ---- HARD GATE (scaled by sensitivity knob) ----
    const float gate = BAND_GATE_BASE[b] * sensMul;
    if (over < gate) over = 0;

    // Envelope (attack/release smoothing)
    float a = (over > bandEnv[b]) ? ATTACK_A : RELEASE_A;
    bandEnv[b] = bandEnv[b] + a*(over - bandEnv[b]);

    // Hysteresis-based on/off (useful if you want boolean pixels)
    if (!bandOn[b]) bandOn[b] = (bandEnv[b] > gate + HYST_DELTA);
    else            bandOn[b] = (bandEnv[b] > gate - HYST_DELTA);

    outBands[b] = bandEnv[b];
  }
}

/*** Color palette (sub->treble = green->red) ***/
static CRGB bandColor(int idx) {
  switch (idx) {
    case 0: return CRGB(0,255,0);
    case 1: return CRGB(128,255,0);
    case 2: return CRGB(255,255,0);
    case 3: return CRGB(255,128,0);
    default:return CRGB(255,0,0);
  }
}

/*** Rendering (true OFF permitted via knob-controlled cutoff) ***/
static void renderBands(const float bands[5], float visualCutoff) {
  for (int i=0;i<NUM_LEDS;i++){
    float v = clamp01(i < 5 ? bands[i] : 0.0f);
    // OFF unless above the live cutoff; avoids faint glow at idle
    uint8_t bri = (v > visualCutoff) ? (uint8_t)(v * 255) : 0;
    leds[i] = bandColor(i);
    leds[i].nscale8_video(bri);
  }
  FastLED.show();
}

/*** Arduino ***/
void setup() {
  Serial.begin(115200);
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN, LED_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(MAX_BRIGHT);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  ATTACK_A  = alpha_for_ms(ATTACK_MS,  FRAME_MS);
  RELEASE_A = alpha_for_ms(RELEASE_MS, FRAME_MS);

  setupI2S();
  adcSetup();

  Serial.println("FFT 5-band (knob-controlled sensitivity & visual cutoff) ready.");
}

void loop() {
  static uint32_t lastMs=0;
  uint32_t now = millis();
  if (now - lastMs < FRAME_MS) return;
  lastMs = now;

  // 1) Read control knobs
  float sensMul, visualCutoff;
  readKnobs(sensMul, visualCutoff);

  // 2) Capture audio and run FFT
  readI2SBlockToFFT();
  FFT.windowing(vReal, FFT_SIZE, FFT_WIN_TYP_HANN, FFT_FORWARD);
  FFT.compute  (vReal, vImag, FFT_SIZE, FFT_FORWARD);

  // 3) Compute bands (with gating scaled by knob)
  float bands[5];
  computeBandsCalmed_fromFFT(vReal, vImag, bands, sensMul);

  // 4) Render with live visual cutoff so LEDs can be truly OFF
  renderBands(bands, visualCutoff);

  // 5) Debug output to help tune: Floor, Env, Bri per band
  for (int i=0; i<5; i++) {
    uint8_t bri = (bands[i] > visualCutoff) ? (uint8_t)(bands[i] * 255) : 0;
    Serial.print("Band "); Serial.print(i);
    Serial.print(" | Floor="); Serial.print(bandFloor[i], 4);
    Serial.print(" | Env=");   Serial.print(bands[i], 3);
    Serial.print(" | Gate=");  Serial.print(BAND_GATE_BASE[i] * sensMul, 3);
    Serial.print(" | Cut=");   Serial.print(visualCutoff, 3);
    Serial.print(" | Bri=");   Serial.println(bri);
  }
  Serial.println("----");
}