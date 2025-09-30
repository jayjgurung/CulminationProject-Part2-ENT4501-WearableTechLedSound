#include <Arduino.h>
#include <driver/i2s_std.h>   // NEW I2S API (ESP32 core v3.x)
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

/* === SINGLE KNOB ===
   POT_PIN (GPIO34): controls sensitivity, visual cutoff, and mic gain */
#define POT_PIN 34

CRGB leds[NUM_LEDS];

/*** AUDIO / FFT ***/
const uint32_t SAMPLE_RATE = 16000;
const int      FFT_SIZE    = 1024;          // power of 2
const uint16_t FRAME_MS    = 20;            // ~50 FPS

/*** CALMING / CAR TUNING ***/
static const float TOP_BIN_HZ = 6000.0f;              // Ignore hissiest top
static const float WEIGHT_SLOPE_DB_PER_OCT = -3.0f;   // Pink-ish tilt

// Base post-floor gates
static const float BAND_GATE_BASE[5] ={0.53f, 0.18f, 0.20f, 0.22f, 0.14f }; // b0 was 0.36 { 0.36f, 0.18f, 0.20f, 0.22f, 0.26f }; // { 0.32f, 0.16f, 0.18f, 0.20f, 0.24f }; // band0 was 0.18
// Extra divisors per-band to tame highs
static float BAND_EXTRA_DIV[5] = { 3.60f, 1.10f, 1.10f, 1.15f, 1.80f }; // b0 was 3.00
 //{ 3.00f, 1.10f, 1.10f, 1.15f, 1.80f }; //{ 2.50f, 1.10f, 1.10f, 1.10f, 1.70f }; // was {1.30,1.00,1.05,1.10,1.60}


static const float HYST_DELTA_B[5] = { 0.11f, 0.03f, 0.03f, 0.03f, 0.03f };
// Envelope time-constants

static float ATTACK_MS  = 20.0f;
static float RELEASE_MS = 90.0f;

// Internals from time-constants
static float ATTACK_A, RELEASE_A;
static inline float alpha_for_ms(float tau_ms, float dt_ms) {
  return 1.0f - expf(-dt_ms / fmaxf(tau_ms, 1.0f));
}

// // Hysteresis
// static const float HYST_DELTA = 0.03f;

// Band state
static float bandEnv[5]   = {0};
static float bandFloor[5] = {0.05, 0.06f, 0.03f, 0.03f, 0.03f};
static bool  bandOn[5]    = {false,false,false,false,false};

// Bands (sub→treble)
// OLD:
// static const float edgesHz[6] = { 85, 180, 255, 1024, 2048, 15000 };
// NEW (push lows out of engine rumble / HVAC / handling noise):
static const float edgesHz[6] = { 240, 360, 520, 1200, 2500, 8000 };

/*** ===== NEW I2S DRIVER ===== ***/
static i2s_chan_handle_t i2s_rx = nullptr;

/*** I2S setup ***/
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

/*** FFT buffers ***/
static double vReal[FFT_SIZE];
static double vImag[FFT_SIZE];
ArduinoFFT<double> FFT(vReal, vImag, FFT_SIZE, SAMPLE_RATE);

/*** Utils ***/
static inline float clamp01(float x){ return x<0?0:(x>1?1:x); }
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

/*** Band bin mapping ***/
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

/*** Read one block from I2S, apply micGain ***/
static void readI2SBlockToFFT(float micGain) {
  int32_t raw[FFT_SIZE];
  size_t need = sizeof(raw), got = 0;

  while (got < need) {
    size_t br = 0;
    i2s_channel_read(i2s_rx, ((uint8_t*)raw) + got, need - got, &br, portMAX_DELAY);
    got += br;
  }

  // Convert & DC remove
  float mean = 0.f;
  static int16_t s16[FFT_SIZE];
  for (int i=0;i<FFT_SIZE;i++){
    s16[i] = (int16_t)(raw[i] >> 11);
    mean += s16[i];
  }
  mean /= FFT_SIZE;

  for (int i=0;i<FFT_SIZE;i++){
    float centered = ((float)s16[i] - mean) * micGain;
    vReal[i] = (double)centered;
    vImag[i] = 0.0;
  }
}

/*** ADC setup ***/
static inline void adcSetup() {
  analogReadResolution(12); // 0..4095
  analogSetPinAttenuation(POT_PIN, ADC_11db);
  pinMode(POT_PIN, INPUT);
}

/*** Read knob (GPIO34) → sensMul, visCut, micGain ***/
static inline void readKnob(float &sensMul, float &visCut, float &micGain) {
  uint32_t acc = 0;
  for (int i=0;i<4;i++) acc += analogRead(POT_PIN);
  float x = (acc / 4.0f) / 4095.0f;  // 0..1

  // readKnob mapping:
  sensMul = 0.8f  + 1.8f * x;        // was 0.5..3.0  (keeps gates reasonable)
  visCut  = 0.12f + 0.16f * x;       // was 0.08..0.22  (idle needs more headroom)
  micGain = 0.25f * powf(8.0f, x);   // was powf(24.0, x); tames top-end gain
}

/*** Compute bands ***/
static void computeBandsCalmed_fromFFT(double *re, double *im, float outBands[5], const float sensMul) {
  bandsInit();
  float sum[5] = {0};
  float binHz = (float)SAMPLE_RATE / (float)FFT_SIZE;

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

    static const float NORM_SCALE[5] = { 0.00012f, 0.00026f, 0.00028f, 0.00030f, 0.00050f }; // bump band4 // b0 was 0.00016 {0.00016f, 0.00026f, 0.00028f, 0.00030f, 0.00030f }; // { 0.00018f, 0.00026f, 0.00028f, 0.00030f, 0.00030f }; // band0 was 0.00035
    float norm = raw * NORM_SCALE[b] / BAND_EXTRA_DIV[b];
    norm = clamp01(norm);

    // In computeBandsCalmed_fromFFT(): tweak band 0 floor dynamics
    // inside computeBandsCalmed_fromFFT():
    static const float FLOOR_ALPHA_UP[5] = { 0.00012f, 0.0012f, 0.0016f, 0.0016f, 0.0016f }; // slower learn-up b0 (was 0.00025)
    static const float FLOOR_CEIL [5]    = { 0.18f,    0.50f,   0.50f,   0.50f,   0.50f   }; // lower ceiling b0 (was 0.30)
    static const float FLOOR_LEAK[5]     = { 0.00055f, 0.00010f,0.00010f,0.00010f,0.00010f }; // faster leak b0 (was 0.00030)

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

/*** Rendering ***/
// static void renderBands(const float bands[5], float visualCutoff) {
//   for (int i=0;i<NUM_LEDS;i++){
//     float v = clamp01(i < 5 ? bands[i] : 0.0f);
//     uint8_t bri = (v > visualCutoff) ? (uint8_t)(v * 255) : 0;
//     leds[i] = bandColor(i);
//     leds[i].nscale8_video(bri);
//   }
//   FastLED.show();
// }
  static void renderBands(const float bands[5], float visualCutoff) {
    for (int i=0;i<NUM_LEDS;i++){
      float v = clamp01(i < 5 ? bands[i] : 0.0f);
      uint8_t bri = 0;
      if (bandOn[i] && v > visualCutoff) {              // require trigger + cut
        float over = v - visualCutoff;
        float scaled = clamp01(over / (1.0f - visualCutoff));
        bri = (uint8_t)(scaled * 255);
      }
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

  Serial.println("FFT 5-band with single knob (GPIO34: gate/visual + mic gain).");
}

void loop() {
  static uint32_t lastMs=0;
  uint32_t now = millis();
  if (now - lastMs < FRAME_MS) return;
  lastMs = now;

  // 1) Read control knob
  float sensMul, visualCutoff, micGain;
  readKnob(sensMul, visualCutoff, micGain);

  // 2) Capture audio and run FFT
  readI2SBlockToFFT(micGain);
  FFT.windowing(vReal, FFT_SIZE, FFT_WIN_TYP_HANN, FFT_FORWARD);
  FFT.compute  (vReal, vImag, FFT_SIZE, FFT_FORWARD);

  // 3) Compute bands
  float bands[5];
  computeBandsCalmed_fromFFT(vReal, vImag, bands, sensMul);

  // 4) Render
  renderBands(bands, visualCutoff);

  // 5) Debug output
  for (int i=0; i<5; i++) {
    uint8_t bri = (bands[i] > visualCutoff) ? (uint8_t)(bands[i] * 255) : 0;
    Serial.print("Band "); Serial.print(i); Serial.print(" :: ");
    Serial.print(bands[i], 4);   Serial.print(',');
    Serial.print(BAND_GATE_BASE[i] * sensMul, 4); Serial.print(',');
    Serial.print(visualCutoff, 4); Serial.print(',');
    Serial.print(bandFloor[i], 4); Serial.print(',');
    Serial.println((bands[i] > visualCutoff) ? (uint8_t)(bands[0] * 255) : 0);
    //if (i==0) {
       Serial.print(" | MicGain="); Serial.print(micGain, 2); 
       //}
    Serial.println();
  }
  Serial.println("----");
}