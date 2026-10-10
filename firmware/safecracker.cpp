// =====================================================================
// SAFE CRACKER: VAULT BREAKER (RP2040-Zero Port)
//
// A Rogue-Lite Safe Dial Combination Game for RP2040 + AS5600 + ILI9341
// Designed specifically for environments without audio or haptics.
//
// 100% PLAYABLE WITH ROTARY KNOB ONLY (No buttons required!):
//   - Rotate knob on Title Screen to start heist
//   - Rotate knob to dial numbers; hold still in sweet spot for 0.75s to lock pin
//   - Rotate knob to browse Relics; hold still for 1.0s to confirm draft
//   - Rotate knob on Game Over / Victory to restart
//   - Physical buttons (GP26 Action, GP12-15 D-Pad) still supported if wired!
// =====================================================================

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <initializer_list>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"

// ---------------------------------------------------------------------
// Pin Map (RP2040-Zero Outer Header Pins)
// ---------------------------------------------------------------------
#define PIN_SCK     2   // ILI9341 SCK
#define PIN_MOSI    3   // ILI9341 MOSI/SDI
#define PIN_CS      5   // ILI9341 CS
#define PIN_DC      6   // ILI9341 D/C
#define PIN_RST     7   // ILI9341 RST
#define PIN_BL      8   // ILI9341 Backlight

#define PIN_SDA    10   // AS5600 SDA (I2C1)
#define PIN_SCL    11   // AS5600 SCL (I2C1)

#define BTN_UP     12   // Optional D-Pad Up
#define BTN_DOWN   13   // Optional D-Pad Down
#define BTN_LEFT   14   // Optional D-Pad Left (Fallback dial CCW)
#define BTN_RIGHT  15   // Optional D-Pad Right (Fallback dial CW)
#define BTN_ACTION 26   // Optional Action button

static const uint32_t SPI_BAUD   = 62500000; // 62.5 MHz
static const uint8_t  AS5600_ADDR = 0x36;

static const int W = 320, H = 240;
static const float PI  = 3.14159265359f;
static const float TAU = 6.28318530718f;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float randf(float a, float b) { return a + (b - a) * (float(rand()) / float(RAND_MAX)); }

// =====================================================================
// Display Driver (ILI9341, Ping-Pong DMA, 16-bit SPI mode)
// =====================================================================
namespace lcd {
  static inline void cs_low()  { gpio_put(PIN_CS, 0); }
  static inline void cs_high() { gpio_put(PIN_CS, 1); }
  static inline void dc_cmd()  { gpio_put(PIN_DC, 0); }
  static inline void dc_data() { gpio_put(PIN_DC, 1); }

  static int dma_tx = -1;
  static uint16_t dma_buf[2][W];
  static bool inverted = false;

  static void write_cmd(uint8_t cmd) {
    cs_low(); dc_cmd();
    spi_write_blocking(spi0, &cmd, 1);
    cs_high();
  }

  static void cmd_args(uint8_t cmd, std::initializer_list<uint8_t> args) {
    cs_low(); dc_cmd();
    spi_write_blocking(spi0, &cmd, 1);
    if (args.size() > 0) {
      dc_data();
      uint8_t tmp[16];
      size_t i = 0;
      for (uint8_t a : args) tmp[i++] = a;
      spi_write_blocking(spi0, tmp, i);
    }
    cs_high();
  }

  static void set_invert(bool inv) {
    if (inv != inverted) {
      inverted = inv;
      write_cmd(inv ? 0x21 : 0x20); // 0x21 INVON, 0x20 INVOFF
    }
  }

  static void init() {
    gpio_init(PIN_CS);  gpio_set_dir(PIN_CS, GPIO_OUT);  gpio_put(PIN_CS, 1);
    gpio_init(PIN_DC);  gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_init(PIN_RST); gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_init(PIN_BL);  gpio_set_dir(PIN_BL, GPIO_OUT);  gpio_put(PIN_BL, 1);

    spi_init(spi0, SPI_BAUD);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    dma_tx = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(dma_tx, &c, &spi_get_hw(spi0)->dr, nullptr, W, false);

    // Reset sequence
    gpio_put(PIN_RST, 1); sleep_ms(10);
    gpio_put(PIN_RST, 0); sleep_ms(20);
    gpio_put(PIN_RST, 1); sleep_ms(150);

    write_cmd(0x01); sleep_ms(150);
    write_cmd(0x11); sleep_ms(150);

    cmd_args(0x3A, {0x55}); // 16-bit RGB565
    cmd_args(0x36, {0xA8}); // Landscape orientation (rotated 180 degrees)
    write_cmd(0x20);
    cmd_args(0x26, {0x01});
    write_cmd(0x29); sleep_ms(50);
  }

  static void push_frame(const uint8_t *fb, uint16_t ink565, uint16_t paper565) {
    cmd_args(0x2A, {0, 0, (uint8_t)((W - 1) >> 8), (uint8_t)((W - 1) & 0xFF)});
    cmd_args(0x2B, {0, 0, (uint8_t)((H - 1) >> 8), (uint8_t)((H - 1) & 0xFF)});

    cs_low(); dc_cmd();
    uint8_t cmd = 0x2C;
    spi_write_blocking(spi0, &cmd, 1);

    dc_data();
    spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    const uint16_t lut[2] = { paper565, ink565 };
    int buf_idx = 0;

    const uint8_t *src = fb;
    for (int x = 0; x < W; x++) dma_buf[0][x] = lut[src[x]];

    for (int y = 0; y < H; y++) {
      dma_channel_set_read_addr(dma_tx, dma_buf[buf_idx], true);
      if (y + 1 < H) {
        int next_buf = buf_idx ^ 1;
        const uint8_t *next_src = fb + (y + 1) * W;
        for (int x = 0; x < W; x++) dma_buf[next_buf][x] = lut[next_src[x]];
      }
      dma_channel_wait_for_finish_blocking(dma_tx);
      buf_idx ^= 1;
    }

    while (spi_is_busy(spi0)) tight_loop_contents();
    cs_high();
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  }
}

// =====================================================================
// AS5600 Magnetic Encoder
// =====================================================================
namespace knob {
  static bool connected = false;

  static void init() {
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);
  }

  static bool read_raw(uint16_t *out) {
    uint8_t reg = 0x0C;
    if (i2c_write_timeout_us(i2c1, AS5600_ADDR, &reg, 1, true, 2500) < 0) {
      connected = false; return false;
    }
    uint8_t buf[2];
    if (i2c_read_timeout_us(i2c1, AS5600_ADDR, buf, 2, false, 2500) < 0) {
      connected = false; return false;
    }
    *out = ((static_cast<uint16_t>(buf[0]) << 8) | buf[1]) & 0x0FFF;
    connected = true;
    return true;
  }

  static inline bool is_connected() { return connected; }
}

// =====================================================================
// Buttons
// =====================================================================
namespace buttons {
  static void init() {
    for (int pin : {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_ACTION}) {
      gpio_init(pin);
      gpio_set_dir(pin, GPIO_IN);
      gpio_pull_up(pin);
    }
  }
  static inline bool down(int pin) { return !gpio_get(pin); }
}

// =====================================================================
// 1-Bit Software Renderer & Font
// =====================================================================
static uint8_t fb[W * H];
static const uint8_t BAYER[16] = {
  0,  8,  2, 10,
 12,  4, 14,  6,
  3, 11,  1,  9,
 15,  7, 13,  5
};

static inline uint8_t bay(int x, int y) { return BAYER[((y & 3) << 2) | (x & 3)]; }

static inline void px(int x, int y, uint8_t c) {
  if (static_cast<unsigned>(x) < W && static_cast<unsigned>(y) < H) {
    fb[y * W + x] = c;
  }
}

static inline void put(int x, int y, uint8_t c, int shade = -1, bool keep = false) {
  if (static_cast<unsigned>(x) >= W || static_cast<unsigned>(y) >= H) return;
  if (shade < 0) {
    fb[y * W + x] = c;
    return;
  }
  if (bay(x, y) < shade) {
    fb[y * W + x] = 1;
  } else if (!keep) {
    fb[y * W + x] = 0;
  }
}

static void rect(int x, int y, int w, int h, uint8_t c, int shade = -1, bool keep = false) {
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      put(x + i, y + j, c, shade, keep);
    }
  }
}

static void frame_(int x, int y, int w, int h, int t, uint8_t c) {
  rect(x, y, w, t, c);
  rect(x, y + h - t, w, t, c);
  rect(x, y, t, h, c);
  rect(x + w - t, y, t, h, c);
}

static void line(float fx0, float fy0, float fx1, float fy1, uint8_t c) {
  int x0 = static_cast<int>(lroundf(fx0)), y0 = static_cast<int>(lroundf(fy0));
  int x1 = static_cast<int>(lroundf(fx1)), y1 = static_cast<int>(lroundf(fy1));
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    px(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void circle(float fcx, float fcy, int r, uint8_t c) {
  int cx = static_cast<int>(lroundf(fcx)), cy = static_cast<int>(lroundf(fcy));
  int x = 0, y = r, d = 3 - 2 * r;
  while (y >= x) {
    px(cx + x, cy + y, c); px(cx - x, cy + y, c);
    px(cx + x, cy - y, c); px(cx - x, cy - y, c);
    px(cx + y, cy + x, c); px(cx - y, cy + x, c);
    px(cx + y, cy - x, c); px(cx - y, cy - x, c);
    x++;
    if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
    else d = d + 4 * x + 6;
  }
}

static void disc(float fcx, float fcy, int r, uint8_t c, int shade = -1, bool keep = false) {
  int cx = static_cast<int>(lroundf(fcx)), cy = static_cast<int>(lroundf(fcy));
  int r2 = r * r;
  for (int dy = -r; dy <= r; dy++) {
    int dy2 = dy * dy;
    for (int dx = -r; dx <= r; dx++) {
      if (dx * dx + dy2 <= r2) put(cx + dx, cy + dy, c, shade, keep);
    }
  }
}

// 3x5 Bitmap Font
static const char *FONT[256] = {};

static void font_init() {
  auto set = [](char ch, const char *bits) { FONT[static_cast<unsigned char>(ch)] = bits; };
  set('A',"010101111101101"); set('B',"110101110101110"); set('C',"011100100100011"); set('D',"110101101101110");
  set('E',"111100110100111"); set('F',"111100110100100"); set('G',"011100101101011"); set('H',"101101111101101");
  set('I',"111010010010111"); set('J',"001001001101010"); set('K',"101101110101101"); set('L',"100100100100111");
  set('M',"101111111101101"); set('N',"110101101101101"); set('O',"010101101101010"); set('P',"110101110100100");
  set('Q',"010101101110011"); set('R',"110101110101101"); set('S',"011100010001110"); set('T',"111010010010010");
  set('U',"101101101101111"); set('V',"101101101101010"); set('W',"101101111111101"); set('X',"101101010101101");
  set('Y',"101101010010010"); set('Z',"111001010100111");
  set('0',"111101101101111"); set('1',"010110010010111"); set('2',"110001010100111"); set('3',"110001010001110");
  set('4',"101101111001001"); set('5',"111100110001110"); set('6',"011100111101111"); set('7',"111001010001000");
  set('8',"111101111101111"); set('9',"111101111001110");
  set(' ',"000000000000000"); set('.',"000000000000010"); set(':',"000010000010000"); set('-',"000000111000000");
  set('!',"010010010000010"); set('?',"110001010000010"); set('/',"001001010100100"); set('$',"010111010111010");
  set('>',"100010001010100"); set('<',"001010100010001"); set('[',"011010010010011"); set(']',"110010010010110");
  set('*',"101010101010101"); set('#',"101111101111101"); set('%',"101001010100101");
}

static int text_width(const char *s, int scale) {
  size_t len = strlen(s);
  if (len == 0) return 0;
  return static_cast<int>(len) * 4 * scale - scale;
}

enum Align { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

static void text(const char *s, int x, int y, int scale, uint8_t c, Align align = ALIGN_LEFT) {
  if (align == ALIGN_CENTER) x -= text_width(s, scale) / 2;
  else if (align == ALIGN_RIGHT) x -= text_width(s, scale);

  for (int k = 0; s[k]; k++) {
    char ch = s[k];
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    const char *g = FONT[static_cast<unsigned char>(ch)];
    if (!g) g = FONT[static_cast<unsigned char>('.')];
    for (int i = 0; i < 15; i++) {
      if (g[i] == '1') {
        rect(x + k * 4 * scale + (i % 3) * scale, y + (i / 3) * scale, scale, scale, c);
      }
    }
  }
}

// =====================================================================
// Particles (Sparks & Shavings for Mechanical Click Feedback)
// =====================================================================
struct Spark {
  bool alive = false;
  float x = 0, y = 0, vx = 0, vy = 0, life = 0;
};
static const int MAX_SPARKS = 32;
static Spark sparks[MAX_SPARKS];

static void emitSparks(float x, float y, int count) {
  for (int i = 0; i < count; i++) {
    int idx = -1;
    for (int j = 0; j < MAX_SPARKS; j++) {
      if (!sparks[j].alive) { idx = j; break; }
    }
    if (idx < 0) return;
    Spark &s = sparks[idx];
    s.alive = true;
    s.x = x; s.y = y;
    float ang = randf(-PI, PI);
    float spd = randf(20.0f, 65.0f);
    s.vx = cosf(ang) * spd;
    s.vy = sinf(ang) * spd;
    s.life = randf(0.12f, 0.32f);
  }
}

static void updateSparks(float dt) {
  for (int i = 0; i < MAX_SPARKS; i++) {
    if (!sparks[i].alive) continue;
    Spark &s = sparks[i];
    s.x += s.vx * dt;
    s.y += s.vy * dt;
    s.life -= dt;
    if (s.life <= 0) s.alive = false;
    else px(static_cast<int>(s.x), static_cast<int>(s.y), 1);
  }
}

// =====================================================================
// Rogue-Lite Data & Lock State
// =====================================================================
enum DirectionReq { DIR_CW = 1, DIR_CCW = -1 };

struct ComboDigit {
  int targetNumber;       // 0 to 59 dial number
  DirectionReq requiredDir;
  bool unlocked;
};

struct FalseGate {
  int number;             // False gate number that produces fake jitter!
};

static const int MAX_DIGITS = 6;
static const int MAX_FALSE_GATES = 4;

struct VaultDef {
  const char *name;
  int digitsCount;
  int falseGatesCount;
  float tolerance;        // in dial units (0.0 to 1.5)
  float timerSec;
  int payout;
};

static const VaultDef VAULTS[5] = {
  { "LOCAL SAVINGS",    2, 0, 0.85f, 65.0f, 1500 },
  { "MERCHANT DEPOT",   3, 1, 0.70f, 55.0f, 3200 },
  { "DIAMOND EXCHANGE", 3, 2, 0.55f, 48.0f, 5800 },
  { "FEDERAL RESERVE",  4, 2, 0.45f, 42.0f, 9500 },
  { "BLACK CHAMBER",    5, 3, 0.38f, 38.0f, 20000 }
};

enum RelicType {
  RELIC_RESONANCE_COIL,  // Expands jitter detection window by +40%
  RELIC_STETHOSCOPE_KIT, // +1 Durability and Repairs tool
  RELIC_SIGNAL_FILTER,   // False gates show jagged glitch waveform
  RELIC_MAGNETIC_BRAKE,  // Prevents overshooting when slowing down
  RELIC_WIRE_TAPPER,     // +20s time per vault
  RELIC_THERMAL_SCOPE    // Shows arrow hint near target
};

struct RelicInfo {
  RelicType type;
  const char *title;
  const char *desc;
};

static const RelicInfo ALL_RELICS[6] = {
  { RELIC_RESONANCE_COIL,  "RESONANCE COIL",  "+40% WIDER DETECTION WAVE" },
  { RELIC_STETHOSCOPE_KIT, "STETHOSCOPE KIT", "+1 DURABILITY & REPAIR" },
  { RELIC_SIGNAL_FILTER,   "SIGNAL FILTER",   "FALSE GATES DISPLAY GLITCH" },
  { RELIC_MAGNETIC_BRAKE,  "MAGNETIC BRAKE",  "REDUCES DIAL OVERSHOOT" },
  { RELIC_WIRE_TAPPER,     "WIRE TAPPER",     "+20 SEC EXTRA VAULT TIME" },
  { RELIC_THERMAL_SCOPE,   "THERMAL SCOPE",   "SHOWS PROXIMITY ARROW" }
};

enum GameState {
  STATE_TITLE,
  STATE_CRACKING,
  STATE_VAULT_OPENED,
  STATE_RELIC_DRAFT,
  STATE_BUSTED,
  STATE_VICTORY
};

static GameState gameState = STATE_TITLE;
static int currentVaultIdx = 0;
static int currentDigitIdx = 0;
static ComboDigit combo[MAX_DIGITS];
static FalseGate falseGates[MAX_FALSE_GATES];

// Player stats
static int durability = 3;
static int maxDurability = 3;
static int totalLoot = 0;
static float vaultTimeLeft = 60.0f;
static float alarmHeat = 0.0f; // 0 to 100%
static bool activeRelics[6] = { false };
static int relicDraftChoices[3];
static int relicDraftCursor = 0;

// Dial Physics
static float currentDialPos = 0.0f; // 0.0 to 60.0
static float prevDialPos = 0.0f;
static float dialVelocity = 0.0f;
static float needleRebound = 0.0f;  // Top index needle spring bounce
static float screenShake = 0.0f;
static float flashTimer = 0.0f;
static bool wasInSweetSpot = false;

// Knob-Only Navigation & Confirmation Physics
static float dwellTimer = 0.0f;
static const float DWELL_LOCK_TIME = 0.75f; // Hold in sweet spot for 0.75s to drop fence!
static float titleSpinAccum = 0.0f;
static float relicDraftDialAccum = 0.0f;
static float relicHoldTimer = 0.0f;
static const float RELIC_CONFIRM_TIME = 1.0f; // Hold steady on relic for 1.0s to confirm!
static float restartSpinAccum = 0.0f;

// Optional Button edge detection
static bool prevActionBtn = false;
static bool prevUpBtn = false;
static bool prevDownBtn = false;

// Helpers for Safe Dial Math
static inline float diffMod60(float a, float b) {
  float d = fabsf(a - b);
  if (d > 30.0f) d = 60.0f - d;
  return d;
}

// Universal Dial Updater (reads AS5600 or fallback buttons, returns delta movement)
static float updateDialAngle(float dt) {
  uint16_t rawAngle = 0;
  bool encoderOk = knob::read_raw(&rawAngle);
  float diff = 0.0f;

  if (encoderOk) {
    float targetPos = (float(rawAngle) / 4096.0f) * 60.0f;
    diff = targetPos - currentDialPos;
    if (diff > 30.0f) diff -= 60.0f;
    if (diff < -30.0f) diff += 60.0f;

    dialVelocity = (dt > 1e-5f) ? (diff / dt) : 0.0f;
    currentDialPos += diff;
    while (currentDialPos >= 60.0f) currentDialPos -= 60.0f;
    while (currentDialPos < 0.0f) currentDialPos += 60.0f;
  } else {
    // FALLBACK: Use D-Pad Left/Right if AS5600 is not wired
    float turnSpeed = 16.0f;
    if (buttons::down(BTN_LEFT))  diff -= turnSpeed * dt;
    if (buttons::down(BTN_RIGHT)) diff += turnSpeed * dt;
    dialVelocity = (dt > 1e-5f) ? (diff / dt) : 0.0f;
    currentDialPos += diff;
    while (currentDialPos >= 60.0f) currentDialPos -= 60.0f;
    while (currentDialPos < 0.0f) currentDialPos += 60.0f;
  }
  return diff;
}

static void generateVault(int vIdx) {
  const VaultDef &vd = VAULTS[vIdx];
  currentDigitIdx = 0;
  vaultTimeLeft = vd.timerSec + (activeRelics[RELIC_WIRE_TAPPER] ? 20.0f : 0.0f);
  alarmHeat = 0.0f;
  dwellTimer = 0.0f;

  DirectionReq dir = DIR_CW;
  int lastNum = static_cast<int>(currentDialPos);

  for (int i = 0; i < vd.digitsCount; i++) {
    int num;
    do {
      num = rand() % 60;
    } while (diffMod60(num, lastNum) < 8.0f); // Ensure at least 8 ticks separation
    combo[i].targetNumber = num;
    combo[i].requiredDir = dir;
    combo[i].unlocked = false;
    lastNum = num;
    dir = (dir == DIR_CW) ? DIR_CCW : DIR_CW; // Alternate direction
  }

  for (int i = 0; i < vd.falseGatesCount; i++) {
    falseGates[i].number = rand() % 60;
  }
}

static void startNewGame() {
  durability = 3;
  maxDurability = 3;
  totalLoot = 0;
  currentVaultIdx = 0;
  for (int i = 0; i < 6; i++) activeRelics[i] = false;
  generateVault(currentVaultIdx);
  gameState = STATE_CRACKING;
}

// =====================================================================
// Visual Renderers: Dial, Waveform Seismograph, and Tumblers
// =====================================================================

static const float DIAL_CX = 92.0f;
static const float DIAL_CY = 112.0f;
static const float DIAL_R  = 54.0f;

static void drawSafeDial(float dialPos, float proximity, bool inSweetSpot, bool isFalseGate) {
  // 1. Outer Knurled Bezel
  disc(DIAL_CX, DIAL_CY, static_cast<int>(DIAL_R + 6), 1, 3);
  circle(DIAL_CX, DIAL_CY, static_cast<int>(DIAL_R + 6), 1);
  circle(DIAL_CX, DIAL_CY, static_cast<int>(DIAL_R + 1), 1);

  // 2. Dial Face with subtle circular gradient shading
  disc(DIAL_CX, DIAL_CY, static_cast<int>(DIAL_R), 0);
  disc(DIAL_CX, DIAL_CY, static_cast<int>(DIAL_R - 10), 1, 1, true);

  // 3. Tick Marks (Every tick = 6 degrees, total 60 ticks)
  for (int i = 0; i < 60; i++) {
    float tickAngle = -PI * 0.5f - ((float(i) - dialPos) / 60.0f) * TAU;
    float cosA = cosf(tickAngle);
    float sinA = sinf(tickAngle);

    bool isMajor = (i % 5 == 0);
    float rOuter = DIAL_R - 1.0f;
    float rInner = isMajor ? (DIAL_R - 8.0f) : (DIAL_R - 4.0f);

    line(DIAL_CX + cosA * rInner, DIAL_CY + sinA * rInner,
         DIAL_CX + cosA * rOuter, DIAL_CY + sinA * rOuter, 1);

    if (isMajor && (i % 10 == 0)) {
      char numBuf[4];
      snprintf(numBuf, sizeof(numBuf), "%d", i);
      float rText = DIAL_R - 14.0f;
      text(numBuf, static_cast<int>(DIAL_CX + cosA * rText),
           static_cast<int>(DIAL_CY + sinA * rText) - 2, 1, 1, ALIGN_CENTER);
    }
  }

  // 4. Center Handle Disc
  disc(DIAL_CX, DIAL_CY, 16, 1, 6);
  circle(DIAL_CX, DIAL_CY, 16, 1);
  circle(DIAL_CX, DIAL_CY, 4, 1);

  // 5. TOP INDEX POINTER (The Shear Reference Needle)
  float needleY = DIAL_CY - DIAL_R - 7.0f + needleRebound;
  line(DIAL_CX - 4, needleY - 6, DIAL_CX + 4, needleY - 6, 1);
  line(DIAL_CX - 4, needleY - 6, DIAL_CX, needleY, 1);
  line(DIAL_CX + 4, needleY - 6, DIAL_CX, needleY, 1);
  px(static_cast<int>(DIAL_CX), static_cast<int>(needleY), 1);

  // Optical Contact Flare
  if (inSweetSpot) {
    line(DIAL_CX - 7, needleY, DIAL_CX + 7, needleY, 1);
    circle(DIAL_CX, needleY, 3, 1);
  }

  // 6. Visual Dwell / Locking Progress Bar (Center of dial)
  if (dwellTimer > 0.05f) {
    float progress = clampf(dwellTimer / DWELL_LOCK_TIME, 0.0f, 1.0f);
    int barW = 38;
    int fillW = static_cast<int>(progress * barW);
    int bx = static_cast<int>(DIAL_CX) - barW / 2;
    int by = static_cast<int>(DIAL_CY) - 4;
    rect(bx, by, barW, 8, 0);
    frame_(bx, by, barW, 8, 1, 1);
    rect(bx + 1, by + 1, fillW, 6, 1);
  }
}

// Tumbler Cutaway Cross-Section (Right side HUD: X: 190..312, Y: 30..128)
static void drawTumblerChamber(int totalDigits, int activeIdx, float dialPos) {
  frame_(188, 32, 126, 96, 1, 1);
  rect(189, 33, 124, 11, 1, 12);
  text("MECHANICAL TUMBLER", 251, 35, 1, 1, ALIGN_CENTER);

  // Overhead Fence Bar (The lever that drops into gate notches)
  float fenceDrop = wasInSweetSpot ? (3.0f + (dwellTimer / DWELL_LOCK_TIME) * 3.0f) : 0.0f;
  line(196, 52 + fenceDrop, 306, 52 + fenceDrop, 1);
  line(196, 53 + fenceDrop, 306, 53 + fenceDrop, 1);
  line(196, 48, 196, 54 + fenceDrop, 1);

  int startX = 200;
  int wheelSpacing = (totalDigits > 1) ? (98 / totalDigits) : 40;

  for (int i = 0; i < totalDigits; i++) {
    int wx = startX + i * wheelSpacing + wheelSpacing / 2;
    int wy = 86;

    if (combo[i].unlocked) {
      rect(wx - 8, wy - 18, 16, 36, 1, 8);
      frame_(wx - 8, wy - 18, 16, 36, 1, 1);
      text("SET", wx, wy - 2, 1, 1, ALIGN_CENTER);
    } else if (i == activeIdx) {
      rect(wx - 7, wy - 18, 14, 36, 0);
      frame_(wx - 7, wy - 18, 14, 36, 1, 1);

      float diff = diffMod60(dialPos, combo[i].targetNumber);
      int notchOffset = static_cast<int>(clampf(diff * 2.2f, 0.0f, 16.0f));

      if (wasInSweetSpot) {
        rect(wx - 4, wy - 18, 8, 7, 0);
        line(wx - 4, wy - 18, wx - 4, wy - 11, 1);
        line(wx + 3, wy - 18, wx + 3, wy - 11, 1);
        line(wx - 4, wy - 11, wx + 3, wy - 11, 1);
        text("*", wx, wy + 2, 1, 1, ALIGN_CENTER);
      } else {
        line(wx - 5, wy - 18 + notchOffset, wx + 5, wy - 18 + notchOffset, 1);
      }

      char idxBuf[4];
      snprintf(idxBuf, sizeof(idxBuf), "#%d", i + 1);
      text(idxBuf, wx, wy + 20, 1, 1, ALIGN_CENTER);
    } else {
      rect(wx - 6, wy - 16, 12, 32, 1, 2);
      frame_(wx - 6, wy - 16, 12, 32, 1, 1);
      text("?", wx, wy - 2, 1, 1, ALIGN_CENTER);
    }
  }
}

// Acoustic Seismograph Waveform (Bottom HUD: X: 8..312, Y: 172..234)
static void drawAcousticWaveform(float proximity, bool inSweetSpot, bool isFalseGate, float time) {
  frame_(8, 172, 304, 62, 1, 1);
  rect(9, 173, 302, 11, 1, 12);
  text("ACOUSTIC STETHOSCOPE // RESONANCE SPECTRUM", 160, 175, 1, 1, ALIGN_CENTER);

  int midY = 205;
  line(12, midY, 308, midY, 1);

  float amp = proximity * 18.0f;
  float freq = 0.25f + proximity * 1.8f;

  int prevX = 12, prevY = midY;

  for (int x = 12; x <= 308; x += 2) {
    float fx = static_cast<float>(x);
    float yOffset = 0.0f;

    if (inSweetSpot) {
      float phase = (fx * 0.8f) + time * 45.0f;
      yOffset = (sinf(phase) > 0 ? 1.0f : -1.0f) * (15.0f + sinf(phase * 0.3f) * 4.0f);
    } else if (proximity > 0.05f) {
      if (isFalseGate && activeRelics[RELIC_SIGNAL_FILTER]) {
        yOffset = sinf(fx * 0.6f + time * 30.0f) * amp * 0.6f +
                  (static_cast<int>(fx) % 6 == 0 ? 6.0f : -6.0f);
      } else {
        float harmonic1 = sinf(fx * freq + time * 20.0f);
        float harmonic2 = sinf(fx * freq * 2.3f - time * 15.0f) * 0.5f;
        yOffset = (harmonic1 + harmonic2) * amp;
      }
    } else {
      yOffset = sinf(fx * 0.08f + time * 4.0f) * 1.5f;
    }

    int currY = static_cast<int>(clampf(midY + yOffset, 185.0f, 226.0f));
    line(prevX, prevY, x, currY, 1);
    prevX = x;
    prevY = currY;
  }

  if (inSweetSpot) {
    rect(95, 196, 130, 13, 0);
    frame_(95, 196, 130, 13, 1, 1);
    if (dwellTimer > 0.05f) {
      char lockBuf[24];
      int pct = static_cast<int>((dwellTimer / DWELL_LOCK_TIME) * 100.0f);
      snprintf(lockBuf, sizeof(lockBuf), "LOCKING... %d%%", pct);
      text(lockBuf, 160, 199, 1, 1, ALIGN_CENTER);
    } else {
      text("[GATE] HOLD STILL TO SET", 160, 199, 1, 1, ALIGN_CENTER);
    }
  } else if (proximity > 0.6f) {
    text("RESONANCE DETECTED", 160, 224, 1, 1, ALIGN_CENTER);
  }
}

// Top Status Bar: Vault, Durability, Alarm Timer
static void drawTopHUD(const VaultDef &vd) {
  char vBuf[32];
  snprintf(vBuf, sizeof(vBuf), "V%d: %s", currentVaultIdx + 1, vd.name);
  text(vBuf, 8, 8, 1, 1);

  text("PICKS:", 180, 8, 1, 1);
  for (int i = 0; i < maxDurability; i++) {
    int px_pos = 222 + i * 8;
    if (i < durability) {
      rect(px_pos, 8, 5, 8, 1);
    } else {
      frame_(px_pos, 8, 5, 8, 1, 1);
    }
  }

  int timerSec = static_cast<int>(ceilf(vaultTimeLeft));
  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02d SEC", timerSec);
  text(timeBuf, 312, 8, 1, 1, ALIGN_RIGHT);

  DirectionReq reqDir = combo[currentDigitIdx].requiredDir;
  const char *dirStr = (reqDir == DIR_CW) ? "TURN CLOCKWISE >>" : "<< TURN COUNTER-CW";
  text(dirStr, 92, 17, 1, 1, ALIGN_CENTER);

  line(8, 26, 312, 26, 1);
}

// =====================================================================
// Main Game Logic Loop
// =====================================================================
static void updateGame(float dt) {
  const VaultDef &vd = VAULTS[currentVaultIdx];

  // 1. Read Knob / Encoder
  float diff = updateDialAngle(dt);

  // 2. Check Proximity to Current Combination Target
  ComboDigit &curTarget = combo[currentDigitIdx];
  float targetNum = static_cast<float>(curTarget.targetNumber);
  float distToTarget = diffMod60(currentDialPos, targetNum);

  float detectWindow = (activeRelics[RELIC_RESONANCE_COIL] ? 5.2f : 3.8f);
  float sweetTolerance = vd.tolerance;

  float proximity = 0.0f;
  bool inSweetSpot = false;
  bool isFalseGateMatch = false;

  if (distToTarget < detectWindow) {
    proximity = 1.0f - (distToTarget / detectWindow);
    if (distToTarget <= sweetTolerance) {
      inSweetSpot = true;
    }
  }

  if (!inSweetSpot) {
    for (int i = 0; i < vd.falseGatesCount; i++) {
      float fDist = diffMod60(currentDialPos, static_cast<float>(falseGates[i].number));
      if (fDist < detectWindow) {
        float fProx = 1.0f - (fDist / detectWindow);
        if (fProx > proximity) {
          proximity = fProx;
          isFalseGateMatch = true;
          if (fDist <= sweetTolerance) inSweetSpot = true;
        }
      }
    }
  }

  // 3. SYNCHRONIZED VISUAL CLICK TRIGGERS
  if (inSweetSpot && !wasInSweetSpot) {
    needleRebound = 3.5f;          // Detent needle drops down
    screenShake = 2.0f;            // 1-frame micro-shudder
    emitSparks(DIAL_CX, DIAL_CY - DIAL_R - 5.0f, 6);
  }
  wasInSweetSpot = inSweetSpot;

  if (needleRebound > 0.0f) {
    needleRebound -= dt * 14.0f;
    if (needleRebound < 0.0f) needleRebound = 0.0f;
  }

  // 4. DWELL LOCK MECHANIC (Hold still in sweet spot to drop fence!)
  bool triggerLock = false;
  if (inSweetSpot) {
    dwellTimer += dt;
    if (dwellTimer >= DWELL_LOCK_TIME) {
      triggerLock = true;
      dwellTimer = 0.0f;
    }
  } else {
    dwellTimer = 0.0f;
  }

  // Optional Action Button also triggers lock immediately
  bool actionBtn = buttons::down(BTN_ACTION);
  if (actionBtn && !prevActionBtn) {
    triggerLock = true;
  }
  prevActionBtn = actionBtn;

  // 5. Alarm Timer & Heat Countdown
  vaultTimeLeft -= dt;
  if (vaultTimeLeft <= 0.0f) {
    durability--;
    screenShake = 5.0f;
    lcd::set_invert(true);
    flashTimer = 0.15f;
    if (durability <= 0) {
      gameState = STATE_BUSTED;
      return;
    } else {
      vaultTimeLeft = vd.timerSec;
    }
  }

  if (fabsf(dialVelocity) > 90.0f) {
    alarmHeat += dt * 12.0f;
    if (alarmHeat >= 100.0f) {
      durability--;
      alarmHeat = 0.0f;
      screenShake = 6.0f;
      if (durability <= 0) { gameState = STATE_BUSTED; return; }
    }
  } else {
    alarmHeat = clampf(alarmHeat - dt * 4.0f, 0.0f, 100.0f);
  }

  // 6. PROCESS GATE LOCK EVENT
  if (triggerLock) {
    if (inSweetSpot && !isFalseGateMatch) {
      curTarget.unlocked = true;
      currentDigitIdx++;
      emitSparks(DIAL_CX, DIAL_CY - DIAL_R - 5.0f, 16);
      screenShake = 4.0f;
      lcd::set_invert(true);
      flashTimer = 0.10f;

      if (currentDigitIdx >= vd.digitsCount) {
        totalLoot += vd.payout;
        if (currentVaultIdx + 1 >= 5) {
          gameState = STATE_VICTORY;
        } else {
          relicDraftCursor = 0;
          relicHoldTimer = 0.0f;
          relicDraftDialAccum = 0.0f;
          int picked = 0;
          while (picked < 3) {
            int r = rand() % 6;
            bool duplicate = false;
            for (int k = 0; k < picked; k++) {
              if (relicDraftChoices[k] == r) { duplicate = true; break; }
            }
            if (!duplicate) relicDraftChoices[picked++] = r;
          }
          gameState = STATE_RELIC_DRAFT;
        }
        return;
      }
    } else {
      durability--;
      screenShake = 5.0f;
      emitSparks(DIAL_CX, DIAL_CY - DIAL_R, 10);
      lcd::set_invert(true);
      flashTimer = 0.12f;
      if (durability <= 0) {
        gameState = STATE_BUSTED;
        return;
      }
    }
  }

  if (flashTimer > 0.0f) {
    flashTimer -= dt;
    if (flashTimer <= 0.0f) lcd::set_invert(false);
  }

  if (screenShake > 0.0f) {
    screenShake -= dt * 10.0f;
    if (screenShake < 0.0f) screenShake = 0.0f;
  }

  // 7. RENDER FRAME
  memset(fb, 0, sizeof(fb));

  drawTopHUD(vd);
  drawSafeDial(currentDialPos, proximity, inSweetSpot, isFalseGateMatch);
  drawTumblerChamber(vd.digitsCount, currentDigitIdx, currentDialPos);

  static float simTime = 0.0f;
  simTime += dt;
  drawAcousticWaveform(proximity, inSweetSpot, isFalseGateMatch, simTime);
  updateSparks(dt);

  if (activeRelics[RELIC_THERMAL_SCOPE] && distToTarget < 15.0f) {
    text(distToTarget < 4.0f ? "[HOT]" : "[WARM]", 92, 174, 1, 1, ALIGN_CENTER);
  }
}

// =====================================================================
// Title, Relic Draft, and Game Over Screens (100% Knob Controlled)
// =====================================================================
static void renderTitleScreen(float dt) {
  float diff = updateDialAngle(dt);
  titleSpinAccum += fabsf(diff);

  memset(fb, 0, sizeof(fb));
  frame_(12, 12, 296, 216, 2, 1);
  text("SAFE CRACKER", 160, 36, 3, 1, ALIGN_CENTER);
  text("ROGUE-LITE VAULT BREAKER", 160, 68, 1, 1, ALIGN_CENTER);

  // Rotating Dial graphic on Title Screen
  disc(160, 114, 26, 1, 4);
  circle(160, 114, 26, 1);
  float markAngle = -PI * 0.5f - (currentDialPos / 60.0f) * TAU;
  line(160, 114, 160 + cosf(markAngle) * 26.0f, 114 + sinf(markAngle) * 26.0f, 1);
  circle(160 + cosf(markAngle) * 26.0f, 114 + sinf(markAngle) * 26.0f, 2, 1);

  text("ROTATE KNOB TO START HEIST", 160, 156, 1, 1, ALIGN_CENTER);
  text("HOLD STILL IN SWEET SPOT TO LOCK PINS", 160, 172, 1, 1, ALIGN_CENTER);

  // Progress Bar for knob spin activation
  int barW = 80;
  int fillW = static_cast<int>(clampf(titleSpinAccum / 4.0f, 0.0f, 1.0f) * barW);
  frame_(160 - barW / 2, 196, barW, 9, 1, 1);
  rect(160 - barW / 2 + 1, 197, fillW, 7, 1);

  if (titleSpinAccum > 4.0f || (buttons::down(BTN_ACTION) && !prevActionBtn)) {
    titleSpinAccum = 0.0f;
    startNewGame();
  }
  prevActionBtn = buttons::down(BTN_ACTION);
}

static void renderRelicDraft(float dt) {
  float diff = updateDialAngle(dt);

  // Knob rotation switches cursor
  relicDraftDialAccum += diff;
  if (relicDraftDialAccum > 2.5f) {
    relicDraftCursor = (relicDraftCursor + 1) % 3;
    relicDraftDialAccum = 0.0f;
    relicHoldTimer = 0.0f;
  } else if (relicDraftDialAccum < -2.5f) {
    relicDraftCursor = (relicDraftCursor + 2) % 3;
    relicDraftDialAccum = 0.0f;
    relicHoldTimer = 0.0f;
  }

  // Optional D-Pad support
  bool up = buttons::down(BTN_UP);
  bool down = buttons::down(BTN_DOWN);
  if (up && !prevUpBtn) { relicDraftCursor = (relicDraftCursor + 2) % 3; relicHoldTimer = 0.0f; }
  if (down && !prevDownBtn) { relicDraftCursor = (relicDraftCursor + 1) % 3; relicHoldTimer = 0.0f; }
  prevUpBtn = up; prevDownBtn = down;

  // Holding knob steady confirms choice
  if (fabsf(diff) < 0.2f) {
    relicHoldTimer += dt;
  } else {
    relicHoldTimer = 0.0f;
  }

  memset(fb, 0, sizeof(fb));
  frame_(8, 8, 304, 224, 1, 1);
  text("VAULT CRACKED! CHOOSE A RELIC", 160, 16, 1, 1, ALIGN_CENTER);

  for (int i = 0; i < 3; i++) {
    int cardY = 38 + i * 56;
    const RelicInfo &info = ALL_RELICS[relicDraftChoices[i]];
    bool selected = (relicDraftCursor == i);

    if (selected) {
      rect(16, cardY, 288, 50, 1, 3);
      frame_(16, cardY, 288, 50, 2, 1);
      text(">>", 24, cardY + 12, 1, 1);

      // Confirmation Bar on Selected Card
      int barW = 120;
      int fillW = static_cast<int>(clampf(relicHoldTimer / RELIC_CONFIRM_TIME, 0.0f, 1.0f) * barW);
      frame_(150, cardY + 36, barW, 6, 1, 1);
      rect(151, cardY + 37, fillW, 4, 1);
      text("HOLD STILL", 94, cardY + 35, 1, 1);
    } else {
      frame_(16, cardY, 288, 50, 1, 1);
    }

    text(info.title, 44, cardY + 8, 1, 1);
    text(info.desc, 44, cardY + 22, 1, 1);
  }

  text("TURN KNOB TO SELECT // HOLD STILL TO CONFIRM", 160, 214, 1, 1, ALIGN_CENTER);

  if (relicHoldTimer >= RELIC_CONFIRM_TIME || (buttons::down(BTN_ACTION) && !prevActionBtn)) {
    int chosenRelic = relicDraftChoices[relicDraftCursor];
    activeRelics[chosenRelic] = true;
    if (chosenRelic == RELIC_STETHOSCOPE_KIT) {
      maxDurability++;
      durability = maxDurability;
    }
    currentVaultIdx++;
    generateVault(currentVaultIdx);
    gameState = STATE_CRACKING;
  }
  prevActionBtn = buttons::down(BTN_ACTION);
}

static void renderBusted(float dt) {
  float diff = updateDialAngle(dt);
  restartSpinAccum += fabsf(diff);

  memset(fb, 0, sizeof(fb));
  frame_(16, 20, 288, 200, 2, 1);
  text("BUSTED!", 160, 48, 3, 1, ALIGN_CENTER);
  text("THE ALARM SOUNDED & GUARDS ARRIVED", 160, 85, 1, 1, ALIGN_CENTER);

  char scoreBuf[32];
  snprintf(scoreBuf, sizeof(scoreBuf), "LOOT SECURED: $%d", totalLoot);
  text(scoreBuf, 160, 120, 1, 1, ALIGN_CENTER);

  char vBuf[32];
  snprintf(vBuf, sizeof(vBuf), "VAULTS CLEARED: %d / 5", currentVaultIdx);
  text(vBuf, 160, 140, 1, 1, ALIGN_CENTER);

  text("ROTATE KNOB TO TRY AGAIN", 160, 186, 1, 1, ALIGN_CENTER);

  if (restartSpinAccum > 4.0f || (buttons::down(BTN_ACTION) && !prevActionBtn)) {
    restartSpinAccum = 0.0f;
    gameState = STATE_TITLE;
  }
  prevActionBtn = buttons::down(BTN_ACTION);
}

static void renderVictory(float dt) {
  float diff = updateDialAngle(dt);
  restartSpinAccum += fabsf(diff);

  memset(fb, 0, sizeof(fb));
  frame_(16, 20, 288, 200, 2, 1);
  text("HEIST COMPLETE!", 160, 45, 2, 1, ALIGN_CENTER);
  text("MASTER VAULT CRACKER", 160, 75, 1, 1, ALIGN_CENTER);

  char scoreBuf[32];
  snprintf(scoreBuf, sizeof(scoreBuf), "FINAL HAUL: $%d", totalLoot);
  text(scoreBuf, 160, 115, 2, 1, ALIGN_CENTER);

  text("RATING: S-TIER GHOST BURGLAR", 160, 145, 1, 1, ALIGN_CENTER);
  text("ROTATE KNOB TO PLAY AGAIN", 160, 186, 1, 1, ALIGN_CENTER);

  if (restartSpinAccum > 4.0f || (buttons::down(BTN_ACTION) && !prevActionBtn)) {
    restartSpinAccum = 0.0f;
    gameState = STATE_TITLE;
  }
  prevActionBtn = buttons::down(BTN_ACTION);
}

// =====================================================================
// Main Entry Point
// =====================================================================
int main() {
  stdio_init_all();
  font_init();
  lcd::init();
  knob::init();
  buttons::init();

  const uint16_t PAPER_COLOR = 0x0842; // Deep Slate #080810
  const uint16_t INK_COLOR   = 0x07E0; // Phosphor Emerald Green #00FF00

  uint32_t lastUs = time_us_32();

  while (true) {
    uint32_t nowUs = time_us_32();
    float dt = (nowUs - lastUs) * 1e-6f;
    lastUs = nowUs;
    if (dt > 0.05f) dt = 0.05f;

    switch (gameState) {
      case STATE_TITLE:
        renderTitleScreen(dt);
        break;
      case STATE_CRACKING:
        updateGame(dt);
        break;
      case STATE_RELIC_DRAFT:
        renderRelicDraft(dt);
        break;
      case STATE_BUSTED:
        renderBusted(dt);
        break;
      case STATE_VICTORY:
        renderVictory(dt);
        break;
      default:
        break;
    }

    lcd::push_frame(fb, INK_COLOR, PAPER_COLOR);
  }

  return 0;
}
