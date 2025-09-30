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

CRGB leds[NUM_LEDS];

/*** AUDIO / FFT ***/
const uint32_t SAMPLE_RATE = 16000;
const int      FFT_SIZE    = 1024;          // power of 2
const uint16_t FRAME_MS    = 20;            // ~50 FPS

/*** CALMING / CAR TUNING ***/
static const float TOP_BIN_HZ = 6000.0f;              // ignore the hissiest top
static const float WEIGHT_SLOPE_DB_PER_OCT = -3.0f;   // pink-ish tilt

// Post-floor gates (raise treble gate)
static float BAND_GATE[5]      = {0.10, 0.12, 0.14, 0.16, 0.20};
static float BAND_EXTRA_DIV[5] = { 1.00f,  1.00f,  1.05f,  1.10f,  1.60f };

// Envelope time-constants (snappy)
static float ATTACK_MS  = 20.0f;
static float RELEASE_MS = 70.0f;

// Internals from time-constants
static float ATTACK_A, RELEASE_A;
static inline float alpha_for_ms(float tau_ms, float dt_ms) {
  return 1.0f - expf(-dt_ms / fmaxf(tau_ms, 1.0f));
}

static const float HYST_DELTA = 0.03f; // LED off hysteresis

// Band state
static float bandEnv[5]   = {0};
static float bandFloor[5] = {0.6f, 0.068f, 0.033f, 0.033f, 0.033f}; // seeds; will adapt
static bool  bandOn[5]    = {false,false,false,false,false};

// Bands (sub→treble). Keep lows above engine/DC rumble.
static const float edgesHz[6] = { 85, 180, 255, 1024, 2048, 15000 };

/*** I2S setup ***/
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

/*** Read one block from I2S into vReal/vImag (removes DC) ***/
static void readI2SBlockToFFT() {
  int32_t raw[FFT_SIZE];
  size_t br=0, need=sizeof(raw), got=0;
  uint8_t* p = (uint8_t*)raw;
  while (got < need) {
    i2s_read(I2S_NUM_0, (void*)(p+got), need-got, &br, portMAX_DELAY);
    got += br;
  }
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

/*** Compute 5 calm bands (0..1) ***/
static void computeBandsCalmed_fromFFT(double *re, double *im, float outBands[5]) {
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

    // normalization (car): prevent low band from dominating
    static const float NORM_SCALE[5] = { 0.00018f, 0.00028f, 0.00030f, 0.00030f, 0.00030f };
    float norm = raw * NORM_SCALE[b] / BAND_EXTRA_DIV[b];
    norm = clamp01(norm);

    // adaptive floor with ceiling + tiny leak (car cabin)
    static const float FLOOR_ALPHA_UP[5] = { 0.0006f, 0.0015f, 0.0020f, 0.0020f, 0.0020f };
    static const float FLOOR_CEIL [5]    = { 0.60f,   0.55f,   0.55f,   0.55f,   0.55f   };
    static const float FLOOR_LEAK[5]     = { 0.00010f,0.00008f,0.00008f,0.00008f,0.00008f };

    bandFloor[b] = (1.0f - FLOOR_ALPHA_UP[b]) * bandFloor[b] + FLOOR_ALPHA_UP[b] * norm;
    bandFloor[b] = fminf(bandFloor[b], FLOOR_CEIL[b]);
    bandFloor[b] = fmaxf(0.0f, bandFloor[b] - FLOOR_LEAK[b]);

    float over = norm - bandFloor[b];
    if (over < 0) over = 0;

    // hard gate after floor
    if (over < BAND_GATE[b]) over = 0;

    // envelope
    float a = (over > bandEnv[b]) ? ATTACK_A : RELEASE_A;
    bandEnv[b] = bandEnv[b] + a*(over - bandEnv[b]);

    // hysteresis (optional)
    if (!bandOn[b]) bandOn[b] = (bandEnv[b] > BAND_GATE[b] + HYST_DELTA);
    else            bandOn[b] = (bandEnv[b] > BAND_GATE[b] - HYST_DELTA);

    outBands[b] = bandEnv[b];
  }
}

/*** Colors + rendering (true OFF allowed) ***/
static CRGB bandColor(int idx) {
  switch (idx) {
    case 0: return CRGB(0,255,0);
    case 1: return CRGB(128,255,0);
    case 2: return CRGB(255,255,0);
    case 3: return CRGB(255,128,0);
    default:return CRGB(255,0,0);
  }
}
static void renderBands(const float bands[5]) {
  for (int i=0;i<NUM_LEDS;i++){
    float v = clamp01(i < 5 ? bands[i] : 0.0f);
    // visual gate: ignore tiny values so pixels can be truly OFF
    uint8_t bri = (v > 0.16f) ? (uint8_t)(v * 255) : 0;
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
  Serial.println("FFT 5-band (car preset with treble fix) ready.");
}

void loop() {
  static uint32_t lastMs=0;
  uint32_t now = millis();
  if (now - lastMs < FRAME_MS) return;
  lastMs = now;

  readI2SBlockToFFT();

  FFT.windowing(vReal, FFT_SIZE, FFT_WIN_TYP_HANN, FFT_FORWARD);
  FFT.compute  (vReal, vImag, FFT_SIZE, FFT_FORWARD);

  float bands[5];
  computeBandsCalmed_fromFFT(vReal, vImag, bands);
  renderBands(bands);

  // --- Serial Monitor: floors, bands, and brightness per band ---
  static uint8_t dbg=0;
  // if (++dbg >= 5) { dbg = 0;
  //   Serial.print("Floor: ");
  //   for (int i=0;i<5;i++){ Serial.print(bandFloor[i],4); Serial.print(i<4?", ":"  |  "); }
  //   Serial.print("Bands: ");
  //   for (int i=0;i<5;i++){ Serial.print(bands[i],3);     Serial.print(i<4?", ":"  |  "); }
  //   Serial.print("Bri: ");
  //   for (int i=0;i<5;i++){
  //     uint8_t bri = (bands[i] > 0.06f) ? (uint8_t)(bands[i]*255) : 0;
  //     Serial.print((int)bri); Serial.print(i<4?", ":"\n");
  //   }

  //   // ---- Plotter-friendly (uncomment to use the Arduino Serial Plotter) ----
  //   // for (int i=0;i<5;i++){ Serial.print(bandFloor[i],4); Serial.print('\t'); }
  //   // for (int i=0;i<5;i++){ Serial.print(bands[i],3);     Serial.print(i<4?'\t':'\n'); }
  // }

  // --- Debug: print details for each band ---
  for (int i=0; i<5; i++) {
    uint8_t bri = (bands[i] > 0.06f) ? (uint8_t)(bands[i] * 255) : 0;

    Serial.print("Band ");
    Serial.print(i);
    Serial.print(" | Floor=");
    Serial.print(bandFloor[i], 4);
    Serial.print(" | Env=");
    Serial.print(bands[i], 3);
    Serial.print(" | Bri=");
    Serial.println(bri);
  }
  Serial.println("----");
}