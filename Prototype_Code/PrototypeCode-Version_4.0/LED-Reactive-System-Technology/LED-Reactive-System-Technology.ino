#include <Arduino.h>
#include <driver/i2s_std.h>   // NEW I2S API (ESP32 core v3.x)
#include <FastLED.h>
#include <arduinoFFT.h>

/*** ------------------ PIN DEFINITIONS ------------------ ***/
#define I2S_WS_PIN   25   // LRCLK / WS
#define I2S_SCK_PIN  26   // BCLK / SCK
#define I2S_SD_PIN   33   // DOUT from mic -> ESP32

#define LED_DATA_PIN 22

// ---- LED layout: 5 bands × 5 LEDs each ----
#define NUM_BANDS    5
#define GROUP_SIZE   5
#define NUM_LEDS     (NUM_BANDS * GROUP_SIZE)

#define LED_TYPE     WS2812B
#define LED_ORDER    GRB
#define MAX_BRIGHT   200

CRGB leds[NUM_LEDS];

/*** POTENTIOMETERS ***/
#define POT1_PIN 34   // active: sensitivity control
#define POT2_PIN 35   // reserved for future use
#define POT3_PIN 32   // reserved for future use
#define POT4_PIN 39   // reserved for future use

/*** ------------------ AUDIO / FFT SETTINGS ------------------ ***/
const uint32_t SAMPLE_RATE = 16000;
const int      FFT_SIZE    = 1024;
const uint16_t FRAME_MS    = 20;   // ~50 FPS

/*** SOUND WEIGHTING & GATING PARAMETERS ***/
static const float TOP_BIN_HZ = 6000.0f;              // ignore ultra-high hiss
static const float WEIGHT_SLOPE_DB_PER_OCT = -3.0f;   // pink-ish correction

// Base post-floor gates
static const float BAND_GATE_BASE[NUM_BANDS] = {0.53f, 0.18f, 0.20f, 0.22f, 0.14f};
// Extra divisors per-band to tame highs
static float BAND_EXTRA_DIV[NUM_BANDS] = {3.60f, 1.10f, 1.10f, 1.15f, 1.80f};
static const float HYST_DELTA_B[NUM_BANDS] = {0.11f, 0.03f, 0.03f, 0.03f, 0.03f};

// Envelope time constants
static float ATTACK_MS  = 20.0f;
static float RELEASE_MS = 90.0f;
static float ATTACK_A, RELEASE_A;
static inline float alpha_for_ms(float tau_ms, float dt_ms) {
  return 1.0f - expf(-dt_ms / fmaxf(tau_ms, 1.0f));
}

// Band state variables
static float bandEnv[NUM_BANDS]   = {0};
static float bandFloor[NUM_BANDS] = {0.05f, 0.06f, 0.03f, 0.03f, 0.03f};
static bool  bandOn[NUM_BANDS]    = {false,false,false,false,false};

// Band edges (sub → treble)
static const float edgesHz[NUM_BANDS + 1] = {240, 360, 520, 1200, 2500, 8000};

/*** ------------------ I2S MICROPHONE SETUP ------------------ ***/
static i2s_chan_handle_t i2s_rx = nullptr;

static void setupI2S() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, nullptr, &i2s_rx);   // RX only

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                    I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_SCK_PIN,
      .ws   = (gpio_num_t)I2S_WS_PIN,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)I2S_SD_PIN
    }
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  i2s_channel_init_std_mode(i2s_rx, &std_cfg);
  i2s_channel_enable(i2s_rx);
}

/*** ------------------ FFT SETUP ------------------ ***/
static double vReal[FFT_SIZE];
static double vImag[FFT_SIZE];
ArduinoFFT<double> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

/*** ------------------ UTILITY FUNCTIONS ------------------ ***/
static inline float clamp01(float x){ return x < 0 ? 0 : (x > 1 ? 1 : x); }
static inline float fastMagf(float re, float im) {
  float a = fabsf(re), b = fabsf(im);
  return (a > b) ? (a + 0.4f*b) : (b + 0.4f*a);
}
static inline float pinkWeight(float hz) {
  if (hz < 30.0f) hz = 30.0f;
  float oct = logf(hz/30.0f)/logf(2.0f);
  float db  = WEIGHT_SLOPE_DB_PER_OCT * oct;
  return powf(10.0f, db/20.0f);
}

/*** ------------------ BAND INITIALIZATION ------------------ ***/
static bool bandsInited = false;
static int kTop = 0, kEdge[NUM_BANDS + 1];
static void bandsInit() {
  if (bandsInited) return;
  float binHz = (float)SAMPLE_RATE / (float)FFT_SIZE;
  kTop = (int)floorf(TOP_BIN_HZ / binHz);
  if (kTop > (FFT_SIZE/2 - 1)) kTop = FFT_SIZE/2 - 1;
  for (int i = 0; i < NUM_BANDS + 1; ++i) {
    int k = (int)floorf(edgesHz[i] / binHz);
    if (k < 1) k = 1;
    if (k > kTop) k = kTop;
    kEdge[i] = k;
  }
  bandsInited = true;
}

/*** ------------------ I2S BLOCK READ ------------------ ***/
static void readI2SBlockToFFT(float micGain) {
  int32_t raw[FFT_SIZE];
  size_t need = sizeof(raw), got = 0;

  while (got < need) {
    size_t br = 0;
    i2s_channel_read(i2s_rx, ((uint8_t*)raw) + got, need - got, &br, portMAX_DELAY);
    got += br;
  }

  // Convert and remove DC offset
  float mean = 0.f;
  static int16_t s16[FFT_SIZE];
  for (int i=0; i<FFT_SIZE; i++) {
    s16[i] = (int16_t)(raw[i] >> 11);
    mean += s16[i];
  }
  mean /= FFT_SIZE;

  for (int i=0; i<FFT_SIZE; i++) {
    float centered = ((float)s16[i] - mean) * micGain;
    vReal[i] = (double)centered;
    vImag[i] = 0.0;
  }
}

/*** ------------------ ADC SETUP ------------------ ***/
static inline void adcSetup() {
  analogReadResolution(12); // 0..4095
  analogSetPinAttenuation(POT1_PIN, ADC_11db);
  pinMode(POT1_PIN, INPUT);
  pinMode(POT2_PIN, INPUT);
  pinMode(POT3_PIN, INPUT);
  pinMode(POT4_PIN, INPUT);
  (void)POT2_PIN; (void)POT3_PIN; (void)POT4_PIN;  // silence unused warnings
}

/*** ------------------ POT1 READ ------------------ ***/
static inline void readKnob(float &sensMul, float &visCut, float &micGain) {
  uint32_t acc = 0;
  for (int i=0; i<4; i++) acc += analogRead(POT1_PIN);
  float x = (acc / 4.0f) / 4095.0f;  // normalize 0..1

  // Mapping: smooth control for sensitivity, visual cutoff, and mic gain
  sensMul = 0.8f  + 1.8f * x;
  visCut  = 0.12f + 0.16f * x;
  micGain = 0.25f * powf(8.0f, x);
}

/*** ------------------ BAND PROCESSING ------------------ ***/
static void computeBandsCalmed_fromFFT(double *re, double *im, float outBands[NUM_BANDS], const float sensMul) {
  bandsInit();
  float sum[NUM_BANDS] = {0};
  float binHz = (float)SAMPLE_RATE / (float)FFT_SIZE;

  for (int k=1; k<=kTop; ++k) {
    float hz = k * binHz;
    float mag = fastMagf((float)re[k], (float)im[k]);
    float v = mag * pinkWeight(hz);
    int b=0; while (b<NUM_BANDS && k >= kEdge[b+1]) b++;
    if (b>NUM_BANDS-1) b=NUM_BANDS-1;
    sum[b] += v;
  }

  for (int b=0; b<NUM_BANDS; ++b) {
    int widthBins = max(1, kEdge[b+1] - kEdge[b]);
    float raw  = sum[b] / widthBins;

    static const float NORM_SCALE[NUM_BANDS] = {0.00012f, 0.00026f, 0.00028f, 0.00030f, 0.00050f};
    float norm = raw * NORM_SCALE[b] / BAND_EXTRA_DIV[b];
    norm = clamp01(norm);

    // Floor dynamics
    static const float FLOOR_ALPHA_UP[NUM_BANDS] = {0.00012f, 0.0012f, 0.0016f, 0.0016f, 0.0016f};
    static const float FLOOR_CEIL [NUM_BANDS]    = {0.18f,    0.50f,   0.50f,   0.50f,   0.50f};
    static const float FLOOR_LEAK[NUM_BANDS]     = {0.00055f, 0.00010f,0.00010f,0.00010f,0.00010f};

    bandFloor[b] = (1.0f - FLOOR_ALPHA_UP[b]) * bandFloor[b] + FLOOR_ALPHA_UP[b] * norm;
    bandFloor[b] = fminf(bandFloor[b], FLOOR_CEIL[b]);
    bandFloor[b] = fmaxf(0.0f, bandFloor[b] - FLOOR_LEAK[b]);

    float over = norm - bandFloor[b];
    if (over < 0) over = 0;

    const float gate = BAND_GATE_BASE[b] * sensMul;
    if (over < gate) over = 0;

    float a = (over > bandEnv[b]) ? ATTACK_A : RELEASE_A;
    bandEnv[b] = bandEnv[b] + a*(over - bandEnv[b]);

    outBands[b] = bandEnv[b];

    float h = HYST_DELTA_B[b];
    if (!bandOn[b]) bandOn[b] = (bandEnv[b] > gate + h);
    else            bandOn[b] = (bandEnv[b] > gate - h);
  }
}

/*** ------------------ COLOR + RENDER ------------------ ***/
static CRGB bandColor(int idx) {
  switch (idx) {
    case 0: return CRGB(0,255,0);
    case 1: return CRGB(128,255,0);
    case 2: return CRGB(255,255,0);
    case 3: return CRGB(255,128,0);
    default:return CRGB(255,0,0);
  }
}

static void renderBands(const float bands[NUM_BANDS], float visualCutoff) {
  for (int b=0; b<NUM_BANDS; ++b) {
    float v = clamp01(bands[b]);
    uint8_t bri = 0;
    if (bandOn[b] && v > visualCutoff) {
      float over = v - visualCutoff;
      float scaled = clamp01(over / (1.0f - visualCutoff));
      bri = (uint8_t)(scaled * 255);
    }
    CRGB c = bandColor(b);
    for (int i=0; i<GROUP_SIZE; ++i) {
      int idx = b*GROUP_SIZE + i;
      leds[idx] = c;
      leds[idx].nscale8_video(bri);
    }
  }
  FastLED.show();
}

/*** ------------------ ARDUINO SETUP / LOOP ------------------ ***/
void setup() {
  Serial.begin(115200);
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN, LED_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(MAX_BRIGHT);
  FastLED.setMaxPowerInVoltsAndMilliamps(4, 800);   // protect battery
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  ATTACK_A  = alpha_for_ms(ATTACK_MS, FRAME_MS);
  RELEASE_A = alpha_for_ms(RELEASE_MS, FRAME_MS);

  setupI2S();
  adcSetup();

  // optional LED self-test
  for (int i=0; i<NUM_LEDS; ++i) leds[i] = CRGB::White;
  FastLED.show();
  delay(200);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  Serial.println("FFT 5-band visualizer | 25 LEDs | Pot1 active | Pots2–4 reserved.");
}

void loop() {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < FRAME_MS) return;
  lastMs = now;

  // 1) Read control knob
  float sensMul, visualCutoff, micGain;
  readKnob(sensMul, visualCutoff, micGain);

  // 2) Capture audio + FFT
  readI2SBlockToFFT(micGain);
  FFT.windowing(vReal, FFT_SIZE, FFT_WIN_TYP_HANN, FFT_FORWARD);
  FFT.compute(vReal, vImag, FFT_SIZE, FFT_FORWARD);

  // 3) Compute bands
  float bands[NUM_BANDS];
  computeBandsCalmed_fromFFT(vReal, vImag, bands, sensMul);

  // 4) Render LEDs
  renderBands(bands, visualCutoff);

  // 5) Debug print
  for (int i=0; i<NUM_BANDS; i++) {
    uint8_t bri = (bands[i] > visualCutoff) ? (uint8_t)(bands[i] * 255) : 0;
    Serial.print("Band "); Serial.print(i); Serial.print(" :: ");
    Serial.print(bands[i], 4); Serial.print(',');
    Serial.print(BAND_GATE_BASE[i] * sensMul, 4); Serial.print(',');
    Serial.print(visualCutoff, 4); Serial.print(',');
    Serial.print(bandFloor[i], 4); Serial.print(',');
    Serial.print(bri);
    Serial.print(" | MicGain="); Serial.print(micGain, 2);
    Serial.println();
  }
  Serial.println("----");
}