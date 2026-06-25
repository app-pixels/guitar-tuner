/*
 * app_21_guitar.cpp — GuitarTuner (refactored from 21_GuitarTuner.ino)
 *
 * Portrait 368×448 via Arduino_Canvas.
 * YIN pitch detection, ES8311 codec, 16 kHz microphone.
 *
 * Controls:
 *   BOOT – toggle AUTO mode
 *   PWR  – cycle strings manually (also exits AUTO mode)
 */

#include "app_21_guitar.h"
#include "app_common.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "ESP_I2S.h"
#include "es8311.h"
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include <Adafruit_XCA9554.h>

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;   // allocated once by launcher

// ── Display ───────────────────────────────────────────────────────────────────
static Arduino_Canvas *canvas = nullptr;

// ── GPIO expander ─────────────────────────────────────────────────────────────
static Adafruit_XCA9554 expander;

// ── Canvas dimensions (portrait) ─────────────────────────────────────────────
#define DISP_W  368
#define DISP_H  448

// ── Guitar strings ────────────────────────────────────────────────────────────
struct GStr { const char *name; float freq; int8_t thick; };
static const GStr GSTR[6] = {
  { "e",  82.41f, 3 },
  { "a", 110.00f, 3 },
  { "d", 146.83f, 2 },
  { "g", 196.00f, 2 },
  { "h", 246.94f, 1 },
  { "e", 329.63f, 1 },
};

// ── Layout ────────────────────────────────────────────────────────────────────
static const int16_t STR_Y[6] = { 80, 141, 202, 263, 324, 385 };
#define HDR_LINE_Y   50
#define FOOT_Y      422
#define STR_X0       52
#define STR_X1      350
#define LBL_X        28   // shifted right so 'e' label doesn't overlap the MIC text

// ── Colours ───────────────────────────────────────────────────────────────────
#define COL_BG        0x0000
#define COL_TITLE     0x7BCF
#define COL_DIV       0x2104
#define COL_UNS_LINE  0x2124
#define COL_UNS_LBL   0x528A
#define COL_SEL_LBL   0xFFFF
#define COL_SEL_LINE  0x8410
#define COL_GREEN     0x07E0
#define COL_LIME      0x3FE0
#define COL_YELLOW    0xFFE0
#define COL_ORANGE    0xFD20
#define COL_RED       0xF800
#define COL_FOOT_ACT  0xFFFF
#define COL_FOOT_DIM  0x39C7

// ── Audio ─────────────────────────────────────────────────────────────────────
#define SAMPLE_RATE  16000
#define N_MONO        512
#define N_STEREO     (N_MONO * 2)
#define I2C_CODEC      0

static I2SClass  i2s;
static int16_t   s_stereo[N_STEREO];
static int16_t   s_mono[N_MONO];
static float     s_yin[N_MONO / 2 + 1];

// ── Runtime state ─────────────────────────────────────────────────────────────
static int   g_sel           = 0;
static bool  g_autoMode      = false;
static float g_freq          = 0.0f;
static float g_wave          = 0.0f;
static int   g_autoCandidate = 0;
static int   g_autoCount     = 0;
static bool  bootWas         = false;
static uint32_t lastPwr      = 0;

#define BOOT_PIN  0
#define PWR_MS   50

// ── YIN pitch detection ───────────────────────────────────────────────────────
static float yinDetect(const int16_t *buf, int N, float sr, float thr = 0.15f) {
  int tauMax = N / 2;
  s_yin[0] = 0.0f;
  int lim = N - tauMax;
  for (int t = 1; t <= tauMax; t++) {
    float s = 0.0f;
    for (int j = 0; j < lim; j++) {
      float d = (float)(buf[j] - buf[j + t]);
      s += d * d;
    }
    s_yin[t] = s;
  }
  s_yin[0] = 1.0f;
  float run = 0.0f;
  for (int t = 1; t <= tauMax; t++) {
    run += s_yin[t];
    s_yin[t] = (run > 0.0f) ? (s_yin[t] * (float)t / run) : 1.0f;
  }
  for (int t = 2; t <= tauMax; t++) {
    if (s_yin[t] < thr) {
      while (t + 1 <= tauMax && s_yin[t + 1] < s_yin[t]) t++;
      float tau = (float)t;
      if (t > 1 && t < tauMax) {
        float a = s_yin[t-1], b = s_yin[t], c = s_yin[t+1];
        float den = 2.0f * (2.0f * b - c - a);
        if (fabsf(den) > 1e-10f) tau = (float)t + (c - a) / den;
      }
      return sr / tau;
    }
  }
  return 0.0f;
}

static float calcRMS(const int16_t *buf, int N) {
  float s = 0.0f;
  for (int i = 0; i < N; i++) s += (float)buf[i] * buf[i];
  return sqrtf(s / (float)N);
}

static int nearestString(float freq) {
  int   best  = 0;
  float bestD = fabsf(log2f(freq / GSTR[0].freq));
  for (int i = 1; i < 6; i++) {
    float d = fabsf(log2f(freq / GSTR[i].freq));
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

static uint16_t centsCol(float ac) {
  if (ac <  4.0f) return COL_GREEN;
  if (ac < 12.0f) return COL_LIME;
  if (ac < 25.0f) return COL_YELLOW;
  if (ac < 45.0f) return COL_ORANGE;
  return COL_RED;
}

// ── Draw frame ────────────────────────────────────────────────────────────────
#define WAVE_PERIOD  38.0f
#define WAVE_AMP      6
#define MAX_OFFSET   18
#define TUNE_CENTS    3.0f

static void drawFrame() {
  float target = GSTR[g_sel].freq;
  bool  signal = (g_freq > 10.0f);
  float cents  = signal ? 1200.0f * log2f(g_freq / target) : 0.0f;
  bool  inTune = signal && (fabsf(cents) <= TUNE_CENTS);

  canvas->fillScreen(COL_BG);

  const char *titleStr = "GUITAR TUNER";
  int16_t titleW = (int16_t)strlen(titleStr) * 12;
  canvas->setTextSize(2); canvas->setTextColor(COL_TITLE);
  canvas->setCursor((DISP_W - titleW) / 2, 8);
  canvas->print(titleStr);

  if (signal) {
    if (inTune) {
      // End at x=424 (margin 24) — top row y=8 is in the corner-safe zone
      canvas->setTextSize(2); canvas->setTextColor(COL_GREEN);
      canvas->setCursor(DISP_W - 7 * 12 - 24, 8);
      canvas->print("IN TUNE");
    } else {
      char cbuf[12];
      snprintf(cbuf, sizeof(cbuf), "%+.0f ct", cents);
      canvas->setTextSize(2); canvas->setTextColor(centsCol(fabsf(cents)));
      canvas->setCursor(DISP_W - (int16_t)strlen(cbuf) * 12 - 24, 8);
      canvas->print(cbuf);
    }
  }

  canvas->setTextSize(2);
  canvas->setTextColor(g_autoMode ? COL_GREEN : COL_FOOT_DIM);
  canvas->setCursor(8, 28);
  canvas->print(g_autoMode ? "AUTO" : "MANUAL");

  canvas->drawFastHLine(0, HDR_LINE_Y, DISP_W, COL_DIV);

  for (int i = 0; i < 6; i++) {
    int16_t y   = STR_Y[i];
    bool    sel = (i == g_sel);
    int8_t  th  = GSTR[i].thick;

    canvas->setTextSize(3);
    canvas->setTextColor(sel ? COL_SEL_LBL : COL_UNS_LBL);
    canvas->setCursor(LBL_X, y - 12);
    canvas->print(GSTR[i].name);

    if (!sel) {
      for (int8_t dy = 0; dy < th; dy++)
        canvas->drawFastHLine(STR_X0, y - th / 2 + dy, STR_X1 - STR_X0, COL_UNS_LINE);
    } else if (inTune) {
      for (int8_t dy = -2; dy <= 2; dy++)
        canvas->drawFastHLine(STR_X0, y + dy, STR_X1 - STR_X0, COL_GREEN);
    } else {
      for (int8_t dy = 0; dy < th; dy++)
        canvas->drawFastHLine(STR_X0, y - th / 2 + dy, STR_X1 - STR_X0, COL_SEL_LINE);
      if (signal) {
        float clamped = (cents > 50.0f) ? 50.0f : (cents < -50.0f) ? -50.0f : cents;
        int16_t ofs = -(int16_t)(clamped / 50.0f * (float)MAX_OFFSET);
        uint16_t wc = centsCol(fabsf(cents));
        for (int16_t x = STR_X0; x < STR_X1; x++) {
          float ph = (float)(x - STR_X0) / WAVE_PERIOD * (2.0f * (float)M_PI) + g_wave;
          int16_t wy = y + ofs + (int16_t)((float)WAVE_AMP * sinf(ph));
          if (wy >= HDR_LINE_Y + 2 && wy + 1 < FOOT_Y - 2) {
            canvas->drawPixel(x, wy,     wc);
            canvas->drawPixel(x, wy + 1, wc);
          }
        }
      }
    }
  }

  g_wave += 0.22f;
  if (g_wave > 2.0f * (float)M_PI) g_wave -= 2.0f * (float)M_PI;

  canvas->drawFastHLine(0, FOOT_Y - 2, DISP_W, COL_DIV);

  // ── Tuning-direction arrow ─────────────────────────────────────────────
  // Fixed centre y=222 sits in the safe gap between BOOT pill (~y=75-115)
  // and PWR pill (~y=325-365). Direction encodes cents sign (flat=up,
  // sharp=down); length scales with |cents|.
  {
    const int16_t arrowX  = 348;
    const int16_t centerY = 222;
    const int16_t maxLen  = 80;
    if (signal && !inTune) {
      uint16_t ac = centsCol(fabsf(cents));
      float clampedC = (cents > 50.0f) ? 50.0f : (cents < -50.0f) ? -50.0f : cents;
      int16_t len = (int16_t)(fabsf(clampedC) / 50.0f * (float)maxLen);
      if (len < 14) len = 14;   // minimum visible nub
      bool up = (cents < 0);   // flat → tune up
      // Shaft
      canvas->fillRect(arrowX - 3, up ? centerY - len : centerY, 6, len, ac);
      // Triangle head
      int16_t baseY = up ? (centerY - len)        : (centerY + len);
      int16_t tipY  = up ? (centerY - len - 14)   : (centerY + len + 14);
      canvas->fillTriangle(arrowX - 12, baseY,
                           arrowX + 12, baseY,
                           arrowX,      tipY, ac);
    } else if (signal && inTune) {
      // In tune — solid green dot at centre of arrow zone
      canvas->fillCircle(arrowX, centerY, 10, COL_GREEN);
    }
    // Small chevron on the active string row, just left of the arrow column,
    // so the user can quickly see which string the arrow refers to.
    {
      int16_t rowY = STR_Y[g_sel];
      uint16_t mc = signal && !inTune ? centsCol(fabsf(cents))
                                       : (inTune ? COL_GREEN : COL_SEL_LBL);
      canvas->fillTriangle(arrowX - 18, rowY - 6,
                           arrowX - 18, rowY + 6,
                           arrowX - 8,  rowY,     mc);
    }
  }

  // Pill labels anchored to hardware buttons
  draw_pill_label(canvas, 0, 0, "auto");
  draw_pill_label(canvas, 0, 1, "sel");

  draw_battery_g(canvas, DISP_W, DISP_H);    // top-right, y=10
  draw_watermark_g(canvas, DISP_W, DISP_H); // bottom-centre
  draw_mic_pill(canvas, DISP_W, DISP_H);    // bottom-left red MIC pill

  canvas->flush();
}

// ── ES8311 init ───────────────────────────────────────────────────────────────
static void initCodec() {
  es8311_handle_t h = es8311_create(I2C_CODEC, ES8311_ADDRRES_0);
  if (!h) { USBSerial.println("ES8311 create failed"); return; }
  const es8311_clock_config_t clk = {
    .mclk_inverted      = false,
    .sclk_inverted      = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency     = SAMPLE_RATE * 256,
    .sample_frequency   = SAMPLE_RATE,
  };
  if (es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    USBSerial.println("ES8311 init failed"); return;
  }
  es8311_sample_frequency_config(h, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(h, false);
  es8311_microphone_gain_set(h, (es8311_mic_gain_t)5);
}

// ── App entry points ──────────────────────────────────────────────────────────
void app21_setup(Arduino_OLED *passed_gfx) {
  pinMode(BOOT_PIN, INPUT_PULLUP);

  // PMU already initialised and configured by launcher.

  // Power-cycle DLDO1/2 to reset ES8311 to clean state.
  power.disableDLDO1();
  power.disableDLDO2();
  delay(100);
  power.enableDLDO1(); power.setDLDO1Voltage(1800);
  power.enableDLDO2(); power.setDLDO2Voltage(1800);
  delay(50);   // let rails and ES8311 stabilise

  // Reuse the canvas allocated once by the launcher.
  canvas = g_canvas;

  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);

  i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE,
                 I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO,
                 I2S_STD_SLOT_BOTH))
    USBSerial.println("I2S begin failed");

  initCodec();

  // Reset runtime state
  g_sel           = 0;
  g_autoMode      = false;
  g_freq          = 0.0f;
  g_wave          = 0.0f;
  g_autoCandidate = 0;
  g_autoCount     = 0;
  bootWas         = false;
  lastPwr         = 0;

  drawFrame();
  USBSerial.println("Guitar tuner ready.");
}

void app21_loop() {
  common_tick();
  uint32_t now = millis();

  size_t got = i2s.readBytes((char*)s_stereo, sizeof(s_stereo));
  if (got >= 4) {
    int pairs = (int)got / 4;
    for (int i = 0; i < pairs; i++)
      s_mono[i] = s_stereo[i * 2];
    float level = calcRMS(s_mono, pairs);
    float raw   = 0.0f;
    if (level > 150.0f)
      raw = yinDetect(s_mono, pairs, (float)SAMPLE_RATE);
    if (raw > 20.0f) {
      g_freq = (g_freq < 10.0f) ? raw : 0.4f * raw + 0.6f * g_freq;
    } else {
      g_freq *= 0.90f;
      if (g_freq < 10.0f) g_freq = 0.0f;
    }
  }

  if (g_autoMode && g_freq > 10.0f) {
    int candidate = nearestString(g_freq);
    if (candidate == g_autoCandidate) {
      if (++g_autoCount >= 3) { g_sel = candidate; g_autoCount = 3; }
    } else {
      g_autoCandidate = candidate;
      g_autoCount     = 1;
    }
  }

  bool boot = (digitalRead(BOOT_PIN) == LOW);
  if (boot && !bootWas) {
    common_activity();
    g_autoMode      = !g_autoMode;
    g_autoCandidate = g_sel;
    g_autoCount     = 0;
    g_freq          = 0.0f;
  }
  bootWas = boot;

  if (common_consume_pwr_short()) {
    common_activity();
    if (g_autoMode) {
      g_autoMode = false;
      g_sel      = 0;
    } else {
      g_sel = (g_sel + 1) % 6;
    }
    g_freq = 0.0f;
  }

  drawFrame();
}
