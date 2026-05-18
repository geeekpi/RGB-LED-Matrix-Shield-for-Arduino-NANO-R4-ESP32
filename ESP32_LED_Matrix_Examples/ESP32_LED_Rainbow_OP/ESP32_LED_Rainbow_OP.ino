#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include "Arduino.h"
#include "esp32-hal.h"
#include "esp32-hal-rmt.h"
#include <math.h>

// Default D11 Pin
#define WS2812_PIN D11

// Please modify your wifi's SSID and Password here!
static const char *ssid = "REPLACE_HERE_WITH_YOUR_WIFI_SSID";
static const char *password = "REPLACE_HERE_WITH_YOUR_WIFI_PASSWORD";

// Total 104 pcs RGB LED
#define NUM_LEDS 104
#define BITS_PER_LED 24
#define FRAME_BITS (NUM_LEDS * BITS_PER_LED)

// ====== Matrix Layout Definition ======
#define MATRIX_ROWS 13  // 13 rows
#define MATRIX_COLS 8   // 8 cols

// WS2812 timing @ tick = 100ns
static const uint8_t T0H = 4;
static const uint8_t T0L = 9;
static const uint8_t T1H = 8;
static const uint8_t T1L = 4;

rmt_obj_t *rmtTx = nullptr;
static rmt_data_t rmtBuf[FRAME_BITS];

#define DEBUG_LVL 1
#if DEBUG_LVL
#define DBG(fmt, ...) \
  do { Serial.printf("[DBG] " fmt, ##__VA_ARGS__); } while (0)
#else
#define DBG(fmt, ...) \
  do { \
  } while (0)
#endif

enum Effect : uint8_t {
  EFFECT_SOLID = 0,
  EFFECT_RAINBOW,
  EFFECT_CHASE,
  EFFECT_BREATHE,
  EFFECT_THEATER,
  EFFECT_LARSON,
  EFFECT_TWINKLE,
  EFFECT_HEARTBEAT,    // Heartbeat Heart
  EFFECT_SMILEY,       // New: Yellow Smiley
  EFFECT_SMILEY_BLINK, // New: blinking smiley
  EFFECT_RAINBOW_OP    // New: Rainbow BG + OP Letters
};

volatile Effect g_effect = EFFECT_HEARTBEAT;
volatile bool g_power = true;
volatile uint8_t g_brightness = 128;
volatile int8_t g_dir = +1;
volatile uint8_t g_speed = 5;

// GRB (WS2812B)
static uint8_t g_led_rgb[NUM_LEDS][3];

WebServer server(80);

// ==================== Utility Functions ====================

static inline uint8_t applyBrightness(uint8_t v, uint8_t br) {
  return (uint16_t)v * br / 255;
}

static void hsv2rgb(uint16_t h, uint8_t s, uint8_t v,
                    uint8_t &r, uint8_t &g, uint8_t &b) {
  if (s == 0) {
    r = g = b = v;
    return;
  }
  h %= 360;
  uint16_t region = h / 60;
  uint16_t remainder = (h - region * 60) * 255 / 60;
  uint8_t p = (uint16_t)v * (255 - s) / 255;
  uint8_t q = (uint16_t)v * (255 - ((uint16_t)s * remainder / 255)) / 255;
  uint8_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) / 255)) / 255;
  switch (region) {
    case 0:
      r = v;
      g = t;
      b = p;
      break;
    case 1:
      r = q;
      g = v;
      b = p;
      break;
    case 2:
      r = p;
      g = v;
      b = t;
      break;
    case 3:
      r = p;
      g = q;
      b = v;
      break;
    case 4:
      r = t;
      g = p;
      b = v;
      break;
    default:
      r = v;
      g = p;
      b = q;
      break;
  }
}

// ==================== Coordinate Mapping(Final: Serpentine Column) ====================

/**
 * 2D coord (row, col) to 1D LED index mapping
 * 
 * Final correction based on front red wire marking:
 * - Serpentine column wiring:
 *   * Even cols (0,2,4,6): top to bottom, index increases
 *   * Odd cols (1,3,5,7): bottom to top, index increases
 * - Column connection order: 0→1→2→3→4→5→6→7
 * 
 * Front view index mapping:
 *      col0   col1   col2   col3 | col4   col5   col6   col7
 * Row0: [0]    [25]   [26]   [51] | [52]   [77]   [78]   [103]
 * Row1: [1]    [24]   [27]   [50] | [53]   [76]   [79]   [102]
 * Row2: [2]    [23]   [28]   [49] | [54]   [75]   [80]   [101]
 * Row3: [3]    [22]   [29]   [48] | [55]   [74]   [81]   [100]
 * Row4: [4]    [21]   [30]   [47] | [56]   [73]   [82]   [99]
 * Row5: [5]    [20]   [31]   [46] | [57]   [72]   [83]   [98]
 * Row6: [6]    [19]   [32]   [45] | [58]   [71]   [84]   [97]
 * Row7: [7]    [18]   [33]   [44] | [59]   [70]   [85]   [96]
 * Row8: [8]    [17]   [34]   [43] | [60]   [69]   [86]   [95]
 * Row9: [9]    [16]   [35]   [42] | [61]   [68]   [87]   [94]
 * Row10:[10]   [15]   [36]   [41] | [62]   [67]   [88]   [93]
 * Row11:[11]   [14]   [37]   [40] | [63]   [66]   [89]   [92]
 * Row12:[12]   [13]   [38]   [39] | [64]   [65]   [90]   [91]
 */
static int16_t coordToIndex(int8_t row, int8_t col) {
  if (row < 0 || row >= MATRIX_ROWS || col < 0 || col >= MATRIX_COLS) {
    return -1;
  }

  int16_t base = col * MATRIX_ROWS;  // Column base index

  if (col % 2 == 0) {
    // Even cols (0,2,4,6): top to bottom
    return base + row;
  } else {
    // Odd cols (1,3,5,7): bottom to top
    return base + (MATRIX_ROWS - 1 - row);
  }
}

static void setPixel(int8_t row, int8_t col, uint8_t r, uint8_t g, uint8_t b) {
  int16_t idx = coordToIndex(row, col);
  if (idx >= 0) {
    g_led_rgb[idx][0] = r;
    g_led_rgb[idx][1] = g;
    g_led_rgb[idx][2] = b;
  }
}

static void clearAll() {
  for (int i = 0; i < NUM_LEDS; i++) {
    g_led_rgb[i][0] = 0;
    g_led_rgb[i][1] = 0;
    g_led_rgb[i][2] = 0;
  }
}

// ==================== Yellow Smiley (Red Eyes)====================

// Smiley bitmap (1=yellow face, 2=red eyes, 0=black/off)
// 13 rows x 8 cols
static const uint8_t SMILEY_BITMAP[MATRIX_ROWS][MATRIX_COLS] = {
  {0,0,0,0,0,0,0,0},  // Row 0
  {0,0,1,1,1,1,0,0},  // Row 1: top
  {0,1,1,1,1,1,1,0},  // Row 2: face
  {0,1,2,1,1,2,1,0},  // Row 3: eyes (col 2 and col 5 = red)
  {0,1,1,1,1,1,1,0},  // Row 4: face
  {0,1,2,1,1,2,1,0},  // Row 5: eyes (col 2 and col 5 = red)
  {0,1,1,0,0,1,1,0},  // Row 6: mouth upper
  {0,0,1,1,1,1,0,0},  // Row 7: mouth lower
  {0,0,0,0,0,0,0,0},  // Row 8-12: blank
  {0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0},
};

/**
 * Draw yellow smiley (red eyes)
 */
static void drawSmileyRedEyes() {
  clearAll();

  for (int row = 0; row < MATRIX_ROWS; row++) {
    for (int col = 0; col < MATRIX_COLS; col++) {
      uint8_t pixel = SMILEY_BITMAP[row][col];
      if (pixel == 1) {
        // yellow face
        setPixel(row, col, 255, 255, 0);  // R=255, G=255, B=0
      } else if (pixel == 2) {
        // red eyes
        setPixel(row, col, 255, 0, 0);    // R=255, G=0, B=0
      }
      // pixel == 0 keep off (black)
    }
  }
}
// ==================== Yellow smiley (red eyes + blink animation)====================

static float blinkPhase = 0.0f;

static void drawSmileyRedEyesBlink() {
  clearAll();

  // blink cycle
  blinkPhase += 0.006f * g_speed;
  if (blinkPhase > 1.0f) blinkPhase -= 1.0f;

  bool isBlinking = (blinkPhase > 0.93f);  // last 7% of cycle = eyes closed

  for (int row = 0; row < MATRIX_ROWS; row++) {
    for (int col = 0; col < MATRIX_COLS; col++) {
      uint8_t pixel = SMILEY_BITMAP[row][col];

      if (pixel == 1) {
        // yellow face
        setPixel(row, col, 255, 255, 0);
      } else if (pixel == 2) {
        // eye position
        if (isBlinking) {
          // eyes yellow when closed (blend with face)
          setPixel(row, col, 255, 255, 0);
        } else {
          // Open eyes:red eyes
          setPixel(row, col, 255, 0, 0);
        }
      }
    }
  }
}

// ==================== Heart pattern + heartbeat animation ====================

// Heart bitmap (1=on, 0=off)
// 13 rows x 8 cols，heart on left col 1-2 and right col 5-6
static const uint8_t HEART_BITMAP[MATRIX_ROWS][MATRIX_COLS] = {
  { 0, 0, 0, 0, 0, 0, 0, 0 },  // Row 0
  { 0, 1, 1, 0, 0, 1, 1, 0 },  // Row 1: top two circles
  { 1, 1, 1, 1, 1, 1, 1, 1 },  // Row 2: top circle
  { 1, 1, 1, 1, 1, 1, 1, 1 },  // Row 3: upper part
  { 1, 1, 1, 1, 1, 1, 1, 1 },  // Row 4: widest (middle indent forms heart)
  { 0, 1, 1, 1, 1, 1, 1, 0 },  // Row 5: middle
  { 0, 1, 1, 1, 1, 1, 1, 0 },  // Row 6: lower
  { 0, 0, 1, 1, 1, 1, 0, 0 },  // Row 7: tip
  { 0, 0, 1, 1, 1, 1, 0, 0 },  // Row 8
  { 0, 0, 1, 1, 1, 1, 0, 0 },  // Row 9
  { 0, 0, 0, 1, 1, 0, 0, 0 },  // Row 10
  { 0, 0, 0, 1, 1, 0, 0, 0 },  // Row 11
  { 0, 0, 0, 0, 0, 0, 0, 0 },  // Row 12
};

// Heartbeat animation vars
static float heartbeatPhase = 0.0f;

/**
 * Draw heartbeat heart
 * Effect: red heart brightness pulses, simulating real heartbeat (lub-dub)
 */
static void drawHeartbeat() {
  clearAll();

  // heartbeat speed control
  float beatsPerSecond = 0.5f + (g_speed / 10.0f) * 2.0f;
  float phaseSpeed = beatsPerSecond * 0.016f;

  heartbeatPhase += phaseSpeed;
  if (heartbeatPhase > 1.0f) heartbeatPhase -= 1.0f;

  // Simulate real heartbeat waveform (lub-dub double peak)
  float t = heartbeatPhase * 6.28318f;

  float beat1 = sinf(t);
  if (beat1 < 0) beat1 = 0;
  beat1 = powf(beat1, 0.5f);

  float beat2 = sinf(t - 2.0f);
  if (beat2 < 0) beat2 = 0;
  beat2 = powf(beat2, 0.5f);

  float intensity = beat1 * 0.7f + beat2 * 0.4f;
  if (intensity > 1.0f) intensity = 1.0f;

  float baseBright = 0.3f;
  float pulse = baseBright + (1.0f - baseBright) * intensity;
  pulse = pulse * (g_brightness / 255.0f);

  uint8_t r = (uint8_t)(255 * pulse);

  // Draw heart shape
  for (int row = 0; row < MATRIX_ROWS; row++) {
    for (int col = 0; col < MATRIX_COLS; col++) {
      if (HEART_BITMAP[row][col]) {
        setPixel(row, col, r, 0, 0);
      }
    }
  }
}

// ==================== WS2812 Driver ====================

static void write_one_led_grb(uint8_t g, uint8_t r, uint8_t b, rmt_data_t *out24) {
  uint8_t bytes[3] = { g, r, b };
  int i = 0;
  for (int c = 0; c < 3; c++) {
    for (int bit = 7; bit >= 0; bit--) {
      const bool one = (bytes[c] >> bit) & 0x01;
      out24[i].level0 = 1;
      out24[i].duration0 = one ? T1H : T0H;
      out24[i].level1 = 0;
      out24[i].duration1 = one ? T1L : T0L;
      ++i;
    }
  }
}

static void ws2812_show() {
  if (!g_power) {
    for (int i = 0; i < NUM_LEDS; i++) write_one_led_grb(0, 0, 0, &rmtBuf[i * BITS_PER_LED]);
  } else {
    for (int i = 0; i < NUM_LEDS; i++) {
      uint8_t r = applyBrightness(g_led_rgb[i][0], g_brightness);
      uint8_t g = applyBrightness(g_led_rgb[i][1], g_brightness);
      uint8_t b = applyBrightness(g_led_rgb[i][2], g_brightness);
      write_one_led_grb(g, r, b, &rmtBuf[i * BITS_PER_LED]);
    }
  }
  rmtWrite(rmtTx, rmtBuf, FRAME_BITS);
  delayMicroseconds(300);
}

static void set_all_rgb(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_LEDS; i++) {
    g_led_rgb[i][0] = r;
    g_led_rgb[i][1] = g;
    g_led_rgb[i][2] = b;
  }
}

static uint32_t lastFrameMs = 0;
static uint16_t baseHue = 0;

// ==================== Letter Bitmap Def (Large 13x4)====================
// Bitmap: 1=stroke(rainbow), 0=hollow(off), 2=bg(off)

// Letter O bitmap (13 rows x 4 cols) - fills left half
// 1=stroke (rainbow), 0=hollow (off)
static const uint8_t CHAR_O[MATRIX_ROWS][4] = {
  {0, 1, 1, 0},   // Row 0:  top
  {1, 1, 1, 1},   // Row 1:  upper arc
  {1, 1, 1, 1},   // Row 2:  upper arc
  {1, 1, 1, 1},   // Row 3:  upper arc
  {1, 0, 0, 1},   // Row 4:  left vertical | right vertical
  {1, 0, 0, 1},   // Row 5:  left vertical | right vertical
  {1, 0, 0, 1},   // Row 6:  left vertical | right vertical
  {1, 0, 0, 1},   // Row 7:  left vertical | right vertical
  {1, 0, 0, 1},   // Row 8:  left vertical | right vertical
  {1, 1, 1, 1},   // Row 9:  lower arc
  {1, 1, 1, 1},   // Row 10: lower arc
  {1, 1, 1, 1},   // Row 11: lower arc
  {0, 1, 1, 0},   // Row 12: bottom
};

// Letter P bitmap (13 rows x 4 cols) - fills right half
// 1=stroke (rainbow color), 0=hollow interior (off)
static const uint8_t CHAR_P[MATRIX_ROWS][4] = {
  {1, 0, 0, 0},   // Row 0:  top left vertical
  {1, 0, 0, 0},   // Row 1:  left vertical
  {1, 0, 0, 0},   // Row 2:  left vertical
  {1, 0, 0, 0},   // Row 3:  left vertical
  {1, 0, 0, 0},   // Row 4:  left vertical
  {1, 0, 0, 0},   // Row 5:  left vertical
  {1, 1, 1, 1},   // Row 6:  middle bar (bowl bottom)
  {1, 1, 1, 1},   // Row 7:  middle bar
  {1, 0, 0, 1},   // Row 8:  left vertical | right vertical
  {1, 0, 0, 1},   // Row 9:  left vertical | right vertical
  {1, 0, 0, 1},   // Row 10: left vertical | right vertical
  {1, 1, 1, 1},   // Row 11: bowl top
  {1, 1, 1, 1},   // Row 12: top bar
};

// ==================== Letter Draw Functions (Inverse Hollow)====================

/**
 * Draw letter O (inverse hollow: stroke rainbow, interior off)
 * @param startCol Start column (0-3)
 * @param hueOffset Rainbow hue offset for O/P color difference
 */
static void drawLetterO(int8_t startCol, uint16_t hueOffset) {
  for (int row = 0; row < MATRIX_ROWS; row++) {
    for (int col = 0; col < 4; col++) {
      uint8_t pixel = CHAR_O[row][col];
      if (pixel == 1) {
        // Stroke: rainbow color (by position)
        uint8_t r, g, b;
        uint16_t h = (baseHue + hueOffset + row * 15 + col * 20) % 360;
        hsv2rgb(h, 255, 255, r, g, b);
        setPixel(row, startCol + col, r, g, b);
      }
      // pixel == 0: hollow interior，keep off (black)
    }
  }
}

/**
 * Draw letter P (inverse hollow: stroke rainbow, interior off)
 * @param startCol Start column (4-7)
 * @param hueOffset Rainbow hue offset
 */
static void drawLetterP(int8_t startCol, uint16_t hueOffset) {
  for (int row = 0; row < MATRIX_ROWS; row++) {
    for (int col = 0; col < 4; col++) {
      uint8_t pixel = CHAR_P[row][col];
      if (pixel == 1) {
        // Stroke: rainbow color
        uint8_t r, g, b;
        uint16_t h = (baseHue + hueOffset + row * 15 + col * 20) % 360;
        hsv2rgb(h, 255, 255, r, g, b);
        setPixel(row, startCol + col, r, g, b);
      }
      // pixel == 0: hollow interior，keep off (black)
    }
  }
}

static float chasePos = 0.0f;
static uint8_t theaterOffset = 0;
static float larsonPos = 0.0f;
static int8_t larsonDir = +1;
static float breathPhase = 0.0f;
static uint32_t twinkleSeed = 1234567;

static inline uint32_t xorshift32(uint32_t &s) {
  uint32_t x = s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return s = x;
}

static void update_effect() {
  uint32_t now = millis();
  const uint32_t frameInt = 16;
  if (now - lastFrameMs < frameInt) return;
  lastFrameMs = now;

  const uint8_t spd = g_speed ? g_speed : 1;
  const int8_t dir = (g_dir >= 0) ? +1 : -1;

  switch (g_effect) {
    case EFFECT_SOLID:
      set_all_rgb(255, 0, 0);
      break;

    case EFFECT_RAINBOW:
      {
        const uint8_t sat = 255, val = 255;
        const uint16_t step = 360 / NUM_LEDS;
        for (int i = 0; i < NUM_LEDS; i++) {
          uint8_t r, g, b;
          const uint16_t h = (baseHue + (dir > 0 ? i * step : (NUM_LEDS - 1 - i) * step)) % 360;
          hsv2rgb(h, sat, val, r, g, b);
          g_led_rgb[i][0] = r;
          g_led_rgb[i][1] = g;
          g_led_rgb[i][2] = b;
        }
        baseHue = (uint16_t)((baseHue + dir * spd) % 360);
      }
      break;

    case EFFECT_CHASE:
      {
        set_all_rgb(0, 0, 0);
        chasePos += dir * (0.25f * spd);
        while (chasePos < 0) chasePos += NUM_LEDS;
        while (chasePos >= NUM_LEDS) chasePos -= NUM_LEDS;
        int idx = (int)(chasePos + 0.5f) % NUM_LEDS;
        g_led_rgb[idx][0] = 255;
        g_led_rgb[idx][1] = 255;
        g_led_rgb[idx][2] = 255;
      }
      break;

    case EFFECT_BREATHE:
      {
        float inc = 0.05f * spd;
        breathPhase += inc;
        if (breathPhase > 6.28318f) breathPhase -= 6.28318f;
        float amp = 0.10f + 0.90f * (0.5f * (sinf(breathPhase) + 1.0f));
        uint8_t r = (uint8_t)(255 * amp);
        uint8_t g = 0;
        uint8_t b = 0;
        set_all_rgb(r, g, b);
      }
      break;

    case EFFECT_THEATER:
      {
        set_all_rgb(0, 0, 0);
        for (int i = 0; i < NUM_LEDS; i++) {
          if ((i + theaterOffset) % 3 == 0) {
            g_led_rgb[i][0] = 255;
            g_led_rgb[i][1] = 80;
            g_led_rgb[i][2] = 0;
          }
        }
        theaterOffset = (uint8_t)((theaterOffset + (dir > 0 ? spd : (uint8_t)((3 - (spd % 3)) % 3))) % 3);
      }
      break;

    case EFFECT_LARSON:
      {
        set_all_rgb(0, 0, 0);
        larsonPos += larsonDir * (0.15f * spd);
        if (larsonPos >= (NUM_LEDS - 1)) {
          larsonPos = (float)(NUM_LEDS - 1);
          larsonDir = -larsonDir;
        }
        if (larsonPos <= 0) {
          larsonPos = 0.0f;
          larsonDir = -larsonDir;
        }
        int head = (int)(larsonPos + 0.5f);
        for (int i = 0; i < NUM_LEDS; i++) {
          uint8_t &r = g_led_rgb[i][0], &g = g_led_rgb[i][1], &b = g_led_rgb[i][2];
          r = (uint8_t)((r * 180) / 255);
          g = (uint8_t)((g * 180) / 255);
          b = (uint8_t)((b * 180) / 255);
        }
        g_led_rgb[head][0] = 255;
        g_led_rgb[head][1] = 32;
        g_led_rgb[head][2] = 32;
      }
      break;

    case EFFECT_TWINKLE:
      {
        for (int i = 0; i < NUM_LEDS; i++) {
          for (int c = 0; c < 3; c++) {
            g_led_rgb[i][c] = (uint8_t)((g_led_rgb[i][c] * 200) / 255);
          }
        }
        uint32_t r = xorshift32(twinkleSeed);
        int n = 1 + g_speed / 3;
        for (int k = 0; k < n; k++) {
          int idx = (int)((r + k * 1103515245u) % NUM_LEDS);
          g_led_rgb[idx][0] = 255;
          g_led_rgb[idx][1] = 255;
          g_led_rgb[idx][2] = 255;
        }
      }
      break;

    case EFFECT_HEARTBEAT:
      {
        drawHeartbeat();
      }
      break;

    case EFFECT_SMILEY:
      {
        drawSmileyRedEyes();  // static smiley
      }
      break;

    case EFFECT_SMILEY_BLINK:
      {
        drawSmileyRedEyesBlink();  // blinking smiley
      }
      break;

    // ==================== New: Rainbow + OP Letters (Inverse Hollow)====================
    case EFFECT_RAINBOW_OP:
      {
        // Clear all first (black background)
        clearAll();

        // Draw rainbow letter on left "O" (cols 0-3)
        // hueOffset=0，O shows base rainbow color
        drawLetterO(0, 0);

        // Draw rainbow letter on right "P" (cols 4-7)
        // hueOffset=180，P shows opposite hue rainbow, contrasting with O
        drawLetterP(4, 180);

        // Update rainbow hue for color flow
        baseHue = (uint16_t)((baseHue + dir * spd) % 360);
      }
      break;
  }

  ws2812_show();
}

// ==================== HTML ====================

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Arduino NANO ESP32 Heartbeat LED</title>
<style>
:root{--card-w:700px}
body{font-family:system-ui,Segoe UI,Roboto,Helvetica,Arial,sans-serif;margin:20px;color:#222;background:#f8fafc}
.card{max-width:var(--card-w);margin:auto;padding:18px;border:1px solid #ddd;border-radius:12px;box-shadow:0 2px 10px rgba(0,0,0,.05);background:#fff}
h2{margin:6px 0 16px 0}
.row{display:flex;gap:12px;align-items:center;margin:12px 0}
label{min-width:110px;font-weight:500}
button{padding:8px 14px;border:0;border-radius:8px;cursor:pointer;background:#eee;transition:all .2s}
button:hover{transform:translateY(-1px)}
button.primary{background:#dc2626;color:#fff}
select,input[type="range"]{padding:6px;border-radius:8px;border:1px solid #ccc}
.value{min-width:32px;text-align:right;display:inline-block;font-weight:600;color:#dc2626}
.badge{padding:2px 8px;border-radius:999px;background:#fef2f2;border:1px solid #fecaca;color:#dc2626;font-size:12px}
.heart-preview{font-size:48px;text-align:center;margin:10px 0;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{transform:scale(1)}50%{transform:scale(1.2)}}
</style>
<div class="card">
  <h2>❤️ Arduino Nano ESP32 Heartbeat LED</h2>
  <div class="heart-preview">❤️</div>
  <span id="ip" class="badge"></span>
  <div class="row">
    <label>Power</label>
    <button id="on" class="primary">On</button>
    <button id="off">Off</button>
  </div>
  <div class="row">
    <label>Mode</label>
    <select id="effect">
      <option value="heartbeat" selected>❤️ Heartbeat</option>
      <option value="solid">Solid Red</option>
      <option value="rainbow">Rainbow</option>
      <option value="rainbow_op">🌈 Rainbow OP</option>
      <option value="chase">Chase</option>
      <option value="breathe">Breathe</option>
      <option value="theater">Theater</option>
      <option value="larson">Larson</option>
      <option value="twinkle">Twinkle</option>
      <option value="smiley">😊 Smiley</option>
      <option value="smiley_blink">😉 Blink</option>
    </select>
    <button id="applyEffect">Apply</button>
  </div>
  <div class="row">
    <label>Brightness</label>
    <input type="range" id="bright" min="0" max="255" value="128">
    <span id="bv" class="value">128</span>
  </div>
  <div class="row">
    <label>Heart Rate</label>
    <input type="range" id="speed" min="1" max="10" value="5">
    <span id="sv" class="value">5</span>
    <span class="badge">beats</span>
  </div>
</div>
<script>
const $=(id)=>document.getElementById(id);
const get=(u)=>fetch(u).then(r=>r.text()).catch(()=>{});
const applyState=(s)=>{
  try{
    const j=JSON.parse(s);
    $('bv').textContent = j.brightness;
    $('bright').value   = j.brightness;
    $('sv').textContent = j.speed;
    $('speed').value    = j.speed;
    $('effect').value   = j.effect;
    $('ip').textContent = j.ip || '';
  }catch(_){}
};
$('on').onclick  = ()=>get('/api/power?on=1');
$('off').onclick = ()=>get('/api/power?on=0');
$('applyEffect').onclick= ()=>{
  const e=$('effect').value;
  get('/api/effect?name='+encodeURIComponent(e));
};
$('bright').oninput = ()=> $('bv').textContent = $('bright').value;
$('bright').onchange= ()=> get('/api/brightness?v='+$('bright').value);
$('speed').oninput  = ()=> $('sv').textContent = $('speed').value;
$('speed').onchange = ()=> get('/api/speed?v='+$('speed').value);
get('/api/state').then(applyState);
</script>
</html>
)HTML";

static void sendOKJson(const String &j) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Access-Control-Allow-Origin:", "*");
  server.sendHeader("Access-Control-Allow-Methods: ", "GET");
  server.send(200, "application/json", j);
}
static void sendBad(const String &msg) {
  server.send(400, "application/json", String("{\"error\":\"") + msg + "\"}");
}

void handleRoot() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleState() {
  String j = "{";
  j += "\"power\":" + String(g_power ? "true" : "false");
  j += ",\"effect\":\"" + String(g_effect == EFFECT_SOLID        ? "solid"
                                                                : g_effect == EFFECT_RAINBOW      ? "rainbow"
                                                                : g_effect == EFFECT_RAINBOW_OP   ? "rainbow_op"
                                                                : g_effect == EFFECT_CHASE        ? "chase"
                                                                : g_effect == EFFECT_BREATHE      ? "breathe"
                                                                : g_effect == EFFECT_THEATER      ? "theater"
                                                                : g_effect == EFFECT_LARSON       ? "larson"
                                                                : g_effect == EFFECT_TWINKLE      ? "twinkle"
                                                                : g_effect == EFFECT_SMILEY       ? "smiley"
                                                                : g_effect == EFFECT_SMILEY_BLINK ? "smiley_blink"
                                                                                                  : "heartbeat")
       + "\"";
  j += ",\"brightness\":" + String(g_brightness);
  j += ",\"speed\":" + String(g_speed);
  j += ",\"num_leds\":" + String(NUM_LEDS);
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += "}";
  sendOKJson(j);
}

void handlePower() {
  if (!server.hasArg("on")) return sendBad("missing on");
  g_power = server.arg("on").toInt() != 0;
  ws2812_show();
  sendOKJson(String("{\"power\":") + (g_power ? "true" : "false") + "}");
}

void handleBrightness() {
  if (!server.hasArg("v")) return sendBad("missing v");
  int v = constrain(server.arg("v").toInt(), 0, 255);
  g_brightness = (uint8_t)v;
  ws2812_show();
  sendOKJson(String("{\"brightness\":") + v + "}");
}

void handleSpeed() {
  if (!server.hasArg("v")) return sendBad("missing v");
  int v = constrain(server.arg("v").toInt(), 1, 10);
  g_speed = (uint8_t)v;
  sendOKJson(String("{\"speed\":") + v + "}");
}

void handleEffect() {
  if (!server.hasArg("name")) return sendBad("missing name");
  String n = server.arg("name");
  if (n == "solid") g_effect = EFFECT_SOLID;
  else if (n == "rainbow") g_effect = EFFECT_RAINBOW;
  else if (n == "rainbow_op") g_effect = EFFECT_RAINBOW_OP;
  else if (n == "chase") g_effect = EFFECT_CHASE;
  else if (n == "breathe") g_effect = EFFECT_BREATHE;
  else if (n == "theater") g_effect = EFFECT_THEATER;
  else if (n == "larson") g_effect = EFFECT_LARSON;
  else if (n == "twinkle") g_effect = EFFECT_TWINKLE;
  else if (n == "heartbeat") g_effect = EFFECT_HEARTBEAT;
  else if (n == "smiley") g_effect = EFFECT_SMILEY;
  else if (n == "smiley_blink") g_effect = EFFECT_SMILEY_BLINK;
  else return sendBad("unknown effect");
  sendOKJson(String("{\"effect\":\"") + n + "\"}");
}

void handleNotFound() {
  server.send(404, "text/plain", "404\n");
}

void setup() {
  Serial.begin(115200);
  delay(50);

  rmtTx = rmtInit(WS2812_PIN, RMT_TX_MODE, RMT_MEM_64);
  if (!rmtTx) {
    Serial.println("rmtInit failed");
  }
  float tick = rmtSetTick(rmtTx, 100);
  Serial.printf("RMT tick=%.1f ns, WS2812 on pin %d\n", tick, WS2812_PIN);

  set_all_rgb(0, 0, 0);
  ws2812_show();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to ");
  Serial.println(ssid);

  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    if (++tries == 25) {
      Serial.println("\nretry assoc");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
  }
  Serial.println("\nWiFi connected, IP: " + WiFi.localIP().toString());

  server.on("/", handleRoot);
  server.on("/api/state", handleState);
  server.on("/api/power", handlePower);
  server.on("/api/brightness", handleBrightness);
  server.on("/api/speed", handleSpeed);
  server.on("/api/effect", handleEffect);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  update_effect();
}
