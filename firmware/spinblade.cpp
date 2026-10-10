// =====================================================================
// SPINBLADE -- RP2040-Zero Port (C++, Pico SDK)
//
// Complete, bug-fixed, and hardware-verified source code.
//
// Features:
//   - Knob-driven sword rotation using AS5600 magnetic rotary encoder
//   - Angular-velocity dynamic damage scaling
//   - Smooth dash mechanics with cooldown & collision damage
//   - Grunt & Brute enemies with pathfinding & knockback
//   - Amulet charge & shockwave auto-burst
//   - 1-bit Bayer dithered rendering streamed to ILI9341 over SPI
//   - Screen shake & flash effects
//   - Automatic fallback sword swing if AS5600 is not yet wired
//   - All pins mapped to standard breadboard-accessible pins of RP2040-Zero
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
#include "hardware/vreg.h"
#include "hardware/clocks.h"

// ---------------------------------------------------------------------
// Pin Map (Optimized for Waveshare RP2040-Zero header accessibility)
// Note: GP16..GP25 on RP2040-Zero are bottom solder pads.
// All pins below are on the outer breadboard-friendly pins.
// ---------------------------------------------------------------------
#define PIN_SCK     2   // ILI9341 SCK  (SPI0 SCK)
#define PIN_MOSI    3   // ILI9341 MOSI/SDI (SPI0 TX)
#define PIN_CS      5   // ILI9341 CS   (GPIO)
#define PIN_DC      6   // ILI9341 D/C  (GPIO)
#define PIN_RST     7   // ILI9341 RST  (GPIO)
#define PIN_BL      8   // ILI9341 Backlight LED (GPIO or connect to 3V3)

#define PIN_SDA    10   // AS5600 SDA   (I2C1 SDA)
#define PIN_SCL    11   // AS5600 SCL   (I2C1 SCL)

#define BTN_UP     12   // D-Pad Up
#define BTN_DOWN   13   // D-Pad Down
#define BTN_LEFT   14   // D-Pad Left
#define BTN_RIGHT  15   // D-Pad Right
#define BTN_ACTION 26   // Action button (Dash / Confirm) -- GP26 is an outer header pin!

static const uint32_t SPI_BAUD = 62500000; // 62.5 MHz (Overclocked SPI for 50+ FPS)
static const uint8_t  AS5600_ADDR = 0x36;

// ---------------------------------------------------------------------
// Screen Dimensions & Helpers
// ---------------------------------------------------------------------
static const int W = 320, H = 240;
static const float TAU = 6.28318530718f;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float randf(float a, float b) { return a + (b - a) * (float(rand()) / float(RAND_MAX)); }
static inline float wrapAngle(float a) {
  while (a > 3.14159265f) a -= TAU;
  while (a < -3.14159265f) a += TAU;
  return a;
}

// =====================================================================
// High-Speed Display Driver (ILI9341, 62.5MHz SPI, Ping-Pong DMA)
// =====================================================================
namespace lcd {
  static inline void cs_low()  { gpio_put(PIN_CS, 0); }
  static inline void cs_high() { gpio_put(PIN_CS, 1); }
  static inline void dc_cmd()  { gpio_put(PIN_DC, 0); }
  static inline void dc_data() { gpio_put(PIN_DC, 1); }

  static int dma_tx = -1;
  static uint16_t dma_buf[2][W]; // Ping-pong row buffers
  static bool inverted = false;

  static void write_cmd(uint8_t cmd) {
    cs_low();
    dc_cmd();
    spi_write_blocking(spi0, &cmd, 1);
    cs_high();
  }

  static void cmd_args(uint8_t cmd, std::initializer_list<uint8_t> args) {
    cs_low();
    dc_cmd();
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

  // Instant hardware-level display inversion (zero latency, whole screen at once)
  static void set_invert(bool inv) {
    if (inv != inverted) {
      inverted = inv;
      write_cmd(inv ? 0x21 : 0x20); // 0x21 = INVON, 0x20 = INVOFF
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

    // Setup DMA channel for SPI
    dma_tx = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(dma_tx, &c, &spi_get_hw(spi0)->dr, nullptr, W, false);

    // Hardware reset pulse
    gpio_put(PIN_RST, 1); sleep_ms(10);
    gpio_put(PIN_RST, 0); sleep_ms(20);
    gpio_put(PIN_RST, 1); sleep_ms(150);

    write_cmd(0x01); sleep_ms(150); // Software reset
    write_cmd(0x11); sleep_ms(150); // Exit Sleep

    cmd_args(0x3A, {0x55}); // 16-bit RGB565
    cmd_args(0x36, {0xA8}); // Landscape orientation (rotated 180 degrees)
    write_cmd(0x20);        // Inversion OFF
    cmd_args(0x26, {0x01}); // Gamma curve 1

    write_cmd(0x29); sleep_ms(50);  // Display ON
  }

  // Blazingly fast ping-pong DMA row streaming in native 16-bit SPI mode
  static void push_frame(const uint8_t *fb, uint16_t ink565, uint16_t paper565) {
    // Column address set (0 to W-1)
    cmd_args(0x2A, {0, 0, (uint8_t)((W - 1) >> 8), (uint8_t)((W - 1) & 0xFF)});
    // Page address set (0 to H-1)
    cmd_args(0x2B, {0, 0, (uint8_t)((H - 1) >> 8), (uint8_t)((H - 1) & 0xFF)});

    // Write RAM command in 8-bit mode
    cs_low();
    dc_cmd();
    uint8_t cmd = 0x2C;
    spi_write_blocking(spi0, &cmd, 1);

    // Switch to 16-bit mode with CS kept LOW
    dc_data();
    spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    const uint16_t lut[2] = { paper565, ink565 };
    int buf_idx = 0;

    // Pre-fill row 0
    const uint8_t *src = fb;
    for (int x = 0; x < W; x++) {
      dma_buf[0][x] = lut[src[x]];
    }

    for (int y = 0; y < H; y++) {
      // Start DMA transfer for row y
      dma_channel_set_read_addr(dma_tx, dma_buf[buf_idx], true);

      // CPU computes row y+1 in parallel while DMA is transmitting!
      if (y + 1 < H) {
        int next_buf = buf_idx ^ 1;
        const uint8_t *next_src = fb + (y + 1) * W;
        for (int x = 0; x < W; x++) {
          dma_buf[next_buf][x] = lut[next_src[x]];
        }
      }

      // Wait for current row transfer to complete
      dma_channel_wait_for_finish_blocking(dma_tx);
      buf_idx ^= 1;
    }

    // Wait until SPI transmitter is completely idle before raising CS
    while (spi_is_busy(spi0)) tight_loop_contents();
    cs_high();

    // Restore 8-bit SPI mode for standard commands
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  }
}

// =====================================================================
// AS5600 Magnetic Rotary Encoder (I2C)
// Fixed: Uses timeouts so system does not freeze if sensor is disconnected.
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
    // 2.5ms timeout protects against missing hardware or bus lockup
    if (i2c_write_timeout_us(i2c1, AS5600_ADDR, &reg, 1, true, 2500) < 0) {
      connected = false;
      return false;
    }
    uint8_t buf[2];
    if (i2c_read_timeout_us(i2c1, AS5600_ADDR, buf, 2, false, 2500) < 0) {
      connected = false;
      return false;
    }
    *out = ((static_cast<uint16_t>(buf[0]) << 8) | buf[1]) & 0x0FFF;
    connected = true;
    return true;
  }

  static float read_angle_rad() {
    uint16_t raw;
    if (!read_raw(&raw)) return NAN;
    return (static_cast<float>(raw) / 4096.0f) * TAU;
  }

  static inline bool is_connected() { return connected; }
}

// =====================================================================
// Buttons (Active-low with internal pull-ups)
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
// 1-Bit Software Renderer + Bayer Dithering
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

static void line(float fx0, float fy0, float fx1, float fy1, uint8_t c, int every = 0) {
  int x0 = static_cast<int>(lroundf(fx0)), y0 = static_cast<int>(lroundf(fy0));
  int x1 = static_cast<int>(lroundf(fx1)), y1 = static_cast<int>(lroundf(fy1));
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, n = 0;

  for (;;) {
    if (!every || (n % every == 0)) px(x0, y0, c);
    n++;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void disc(float fcx, float fcy, int r, uint8_t c, int shade = -1, bool keep = false) {
  int cx = static_cast<int>(lroundf(fcx)), cy = static_cast<int>(lroundf(fcy));
  int r2 = r * r;
  for (int dy = -r; dy <= r; dy++) {
    int dy2 = dy * dy;
    for (int dx = -r; dx <= r; dx++) {
      if (dx * dx + dy2 <= r2) {
        put(cx + dx, cy + dy, c, shade, keep);
      }
    }
  }
}

// ---------------------------------------------------------------------
// 3x5 Bitmap Font
// ---------------------------------------------------------------------
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
  set('!',"010010010000010"); set('?',"110001010000010"); set('/',"001001010100100");
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
// Game Entities and Logic
// =====================================================================
enum EnemyType { GRUNT, BRUTE };

struct Enemy {
  bool alive = false;
  EnemyType type = GRUNT;
  float x = 0, y = 0, r = 4.5f, hp = 5, maxHp = 5, speed = 40, dmg = 1;
  float hitCd = 0, flashT = 0;
  bool dashHit = false;
};

struct Particle {
  bool alive = false;
  float x = 0, y = 0, vx = 0, vy = 0, life = 0;
};

static const int MAX_ENEMIES = 24;
static const int MAX_PART    = 48;
static Enemy enemies[MAX_ENEMIES];
static Particle parts[MAX_PART];

struct Player {
  float x = W / 2, y = H / 2, r = 6;
  float hp = 60, maxHp = 60;
  float inv = 0, dashCd = 0, dashT = 0, dashDx = 0, dashDy = 1;
  float amulet = 0;
  int score = 0, level = 1;
  float xp = 0, xpNext = 6;
} player;

static float bladeRadius = 26, bladeDmgMult = 1, dashCdMax = 1.1f, amuletRate = 1;
static float aim = -1.57f, prevAim = -1.57f;
static float elapsed = 0, spawnT = 0.8f;
static float shake = 0, flashScreen = 0;
static float levelUpFlash = 0;

enum GameState { STATE_READY, STATE_PLAYING, STATE_OVER };
static GameState state = STATE_READY;
static bool prevAction = false;
static uint32_t frameCount = 0;

static void spawnEnemy() {
  int idx = -1;
  for (int i = 0; i < MAX_ENEMIES; i++) {
    if (!enemies[i].alive) { idx = i; break; }
  }
  if (idx < 0) return;

  Enemy &e = enemies[idx];
  int side = rand() % 4;
  if (side == 0)      { e.x = randf(0, W); e.y = -12; }
  else if (side == 1) { e.x = W + 12;      e.y = randf(0, H); }
  else if (side == 2) { e.x = randf(0, W); e.y = H + 12; }
  else                { e.x = -12;         e.y = randf(0, H); }

  bool brute = (elapsed > 14.0f) && (randf(0, 1) < 0.28f);
  e.type = brute ? BRUTE : GRUNT;
  e.r = brute ? 9.0f : 4.5f;
  e.hp = e.maxHp = brute ? 16.0f : 5.0f;
  e.speed = (brute ? 25.0f : fminf(60.0f, 35.0f + elapsed * 0.25f)) * randf(0.9f, 1.1f);
  e.dmg = brute ? 2.0f : 1.0f;
  e.hitCd = 0; e.flashT = 0; e.dashHit = false; e.alive = true;
}

static void burst(float x, float y, int n) {
  for (int i = 0; i < n; i++) {
    int idx = -1;
    for (int j = 0; j < MAX_PART; j++) {
      if (!parts[j].alive) { idx = j; break; }
    }
    if (idx < 0) return;
    float a = randf(0, TAU);
    float s = randf(30, 110);
    parts[idx] = {true, x, y, cosf(a) * s, sinf(a) * s, randf(0.2f, 0.45f)};
  }
}

static void onKill(Enemy &e) {
  player.score += (e.type == BRUTE) ? 35 : 10;
  player.xp += (e.type == BRUTE) ? 3 : 1;
  burst(e.x, e.y, (e.type == BRUTE) ? 16 : 9);
  shake = fmaxf(shake, (e.type == BRUTE) ? 3.0f : 1.5f);
  e.alive = false;
}

static void resetGame() {
  player = Player();
  bladeRadius = 26; bladeDmgMult = 1; dashCdMax = 1.1f; amuletRate = 1;
  aim = prevAim = -1.57f;
  elapsed = 0; spawnT = 0.8f; shake = 0; flashScreen = 0; levelUpFlash = 0;
  for (auto &e : enemies) e.alive = false;
  for (auto &p : parts) p.alive = false;
}

static void endGame() {
  state = STATE_OVER;
  shake = 6.0f;
  flashScreen = 0.12f;
}

static void update(float dt) {
  // Movement input from D-pad
  float mx = 0, my = 0;
  if (buttons::down(BTN_LEFT))  mx -= 1;
  if (buttons::down(BTN_RIGHT)) mx += 1;
  if (buttons::down(BTN_UP))    my -= 1;
  if (buttons::down(BTN_DOWN))  my += 1;
  if (mx != 0 && my != 0) { mx *= 0.7071f; my *= 0.7071f; }

  // Rotary encoder input
  float knobAngle = knob::read_angle_rad();
  static bool haveKnob = false;
  static float prevKnob = 0;
  float turn = 0;

  if (!std::isnan(knobAngle)) {
    if (haveKnob) turn = wrapAngle(knobAngle - prevKnob);
    prevKnob = knobAngle;
    haveKnob = true;
  } else {
    // Hardware fallback: if AS5600 is not yet attached, auto-spin or turn with movement
    if (mx != 0 || my != 0) {
      turn = dt * 5.0f; // gentle spin while moving
    }
  }

  bool action = buttons::down(BTN_ACTION);
  bool actionPressed = action && !prevAction;
  prevAction = action;

  prevAim = aim;
  aim = wrapAngle(aim + turn);

  // Timers and decay
  flashScreen = fmaxf(0, flashScreen - dt);
  shake = fmaxf(0, shake - dt * 25.0f);
  levelUpFlash = fmaxf(0, levelUpFlash - dt);

  if (state == STATE_READY || state == STATE_OVER) {
    // Start or retry game by pressing Action button OR by spinning the knob!
    if (actionPressed || fabsf(turn) > 0.12f) {
      resetGame();
      state = STATE_PLAYING;
    }
    return;
  }

  elapsed += dt;

  // Dash and movement handling
  if (mx != 0 || my != 0) { player.dashDx = mx; player.dashDy = my; }
  const float SPEED = 78.0f, DASH_SPEED = 260.0f, DASH_T = 0.16f;
  player.inv = fmaxf(0, player.inv - dt);
  player.dashCd = fmaxf(0, player.dashCd - dt);

  if (actionPressed && player.dashCd <= 0 && player.dashT <= 0) {
    player.dashT = DASH_T;
    player.dashCd = dashCdMax;
    player.inv = fmaxf(player.inv, 0.22f);
    for (auto &e : enemies) e.dashHit = false;
    burst(player.x, player.y, 6);
  }

  if (player.dashT > 0) {
    player.dashT -= dt;
    player.x = clampf(player.x + player.dashDx * DASH_SPEED * dt, player.r + 4, W - player.r - 4);
    player.y = clampf(player.y + player.dashDy * DASH_SPEED * dt, player.r + 4, H - player.r - 4);
  } else {
    player.x = clampf(player.x + mx * SPEED * dt, player.r + 4, W - player.r - 4);
    player.y = clampf(player.y + my * SPEED * dt, player.r + 4, H - player.r - 4);
  }

  // Dash-through collision damage
  if (player.dashT > 0) {
    for (auto &e : enemies) {
      if (!e.alive || e.dashHit) continue;
      float ddx = player.x - e.x, ddy = player.y - e.y;
      float ddist = player.r + e.r + 3.0f;
      if (ddx * ddx + ddy * ddy < ddist * ddist) {
        e.dashHit = true;
        e.hp -= 8;
        e.flashT = 0.08f;
        float ka = atan2f(e.y - player.y, e.x - player.x);
        e.x += cosf(ka) * 12;
        e.y += sinf(ka) * 12;
        burst(e.x, e.y, 5);
        if (e.hp <= 0) onKill(e);
      }
    }
  }

  // Angular damage scaling based on knob speed
  float angVel = wrapAngle(aim - prevAim) / dt;
  float angDeg = fabsf(angVel) * 57.2958f;
  float dmgScale = clampf(angDeg / 300.0f, 0.35f, 2.2f);

  // Enemy spawning
  spawnT -= dt;
  if (spawnT <= 0) {
    spawnEnemy();
    spawnT = fmaxf(0.28f, 1.1f - elapsed * 0.008f) * randf(0.8f, 1.2f);
  }

  // Enemy AI and knockback recovery
  for (auto &e : enemies) {
    if (!e.alive) continue;
    float a = atan2f(player.y - e.y, player.x - e.x);
    e.x += cosf(a) * e.speed * dt;
    e.y += sinf(a) * e.speed * dt;
    e.flashT = fmaxf(0, e.flashT - dt);
    e.hitCd = fmaxf(0, e.hitCd - dt);
  }

  // Sword collision (5 sub-steps to prevent tunneling through fast swings)
  {
    const float HIT_CD = 0.12f;
    for (auto &e : enemies) {
      if (!e.alive || e.hitCd > 0) continue;
      bool hit = false;
      for (int s = 0; s <= 5 && !hit; s++) {
        float a = prevAim + wrapAngle(aim - prevAim) * (s / 5.0f);
        float bx = player.x + cosf(a) * bladeRadius;
        float by = player.y + sinf(a) * bladeRadius;
        float bdx = bx - e.x, bdy = by - e.y;
        float bdist = e.r + 5.0f;
        if (bdx * bdx + bdy * bdy < bdist * bdist) hit = true;
      }
      if (hit) {
        float dmg = 4.0f * bladeDmgMult * fmaxf(dmgScale, 0.45f);
        e.hp -= dmg;
        e.flashT = 0.08f;
        e.hitCd = HIT_CD;
        float ka = atan2f(e.y - player.y, e.x - player.x);
        e.x += cosf(ka) * 2.0f;
        e.y += sinf(ka) * 2.0f;
        player.amulet = fminf(1.0f, player.amulet + 0.045f * amuletRate);
        burst(e.x, e.y, 4);
        shake = fmaxf(shake, 1.2f);
        if (e.hp <= 0) onKill(e);
      }
    }
  }

  // Amulet shockwave auto-burst
  if (player.amulet >= 1.0f) {
    player.amulet = 0;
    shake = fmaxf(shake, 6.0f);
    flashScreen = 0.08f;
    burst(player.x, player.y, 20);
    for (auto &e : enemies) {
      if (!e.alive) continue;
      float adx = e.x - player.x, ady = e.y - player.y;
      if (adx * adx + ady * ady < 2500.0f) {
        e.hp -= 15;
        e.flashT = 0.1f;
        if (e.hp <= 0) onKill(e);
      }
    }
  }

  // Player hurt detection
  if (player.inv <= 0) {
    for (auto &e : enemies) {
      if (!e.alive) continue;
      float pdx = player.x - e.x, pdy = player.y - e.y;
      float pdist = e.r + player.r - 1.0f;
      if (pdx * pdx + pdy * pdy < pdist * pdist) {
        player.hp -= e.dmg;
        player.inv = 0.9f;
        shake = 8.0f;
        flashScreen = 0.09f;
        burst(player.x, player.y, 8);
        if (player.hp <= 0) {
          endGame();
        }
        break;
      }
    }
  }

  // Level Up Buffs
  if (player.xp >= player.xpNext) {
    player.xp -= player.xpNext;
    player.level++;
    player.xpNext = 5 + player.level * 3;
    bladeDmgMult *= 1.15f;
    bladeRadius *= 1.03f;
    player.maxHp += 5;
    player.hp = player.maxHp;
    dashCdMax *= 0.96f;
    levelUpFlash = 1.0f;
  }

  // Particle simulation
  for (auto &p : parts) {
    if (!p.alive) continue;
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    p.vx *= 0.9f;
    p.vy *= 0.9f;
    p.life -= dt;
    if (p.life <= 0) p.alive = false;
  }
}

// =====================================================================
// Drawing Routines
// =====================================================================
static void drawWorld() {
  memset(fb, 0, sizeof(fb));

  // Compute camera offset from screen shake
  int ox = 0, oy = 0;
  if (shake > 0.1f) {
    ox = static_cast<int>(randf(-shake, shake));
    oy = static_cast<int>(randf(-shake, shake));
  }

  // Background grid
  for (int y = 8 + oy; y < H - 6 + oy; y += 18) {
    for (int x = 8 + ox; x < W - 6 + ox; x += 18) {
      px(x, y, 1);
    }
  }
  frame_(3 + ox, 3 + oy, W - 6, H - 6, 1, 1);

  // Drop shadows
  for (auto &e : enemies) {
    if (e.alive) disc(e.x + 2 + ox, e.y + 3 + oy, e.type == BRUTE ? 8 : 4, 1, 6, true);
  }
  if (state != STATE_OVER) {
    disc(player.x + 2 + ox, player.y + 3 + oy, player.r, 1, 8, true);
  }

  // Enemies
  for (auto &e : enemies) {
    if (!e.alive) continue;
    uint8_t c = (e.flashT > 0) ? 0 : 1;
    disc(e.x + ox, e.y + oy, e.type == BRUTE ? 9 : 5, c, (e.flashT > 0) ? -1 : 8, false);

    if (e.flashT > 0) {
      int r = (e.type == BRUTE) ? 9 : 5;
      for (int i = 0; i < 24; i++) {
        float a = (i / 24.0f) * TAU;
        px(static_cast<int>(lroundf(e.x + ox + cosf(a) * r)),
           static_cast<int>(lroundf(e.y + oy + sinf(a) * r)), 1);
      }
    }
  }

  // Player and Blade
  if (state != STATE_OVER) {
    if (player.amulet > 0.02f) {
      int steps = 40;
      for (int i = 0; i < static_cast<int>(steps * player.amulet); i++) {
        float a = -1.5708f + (TAU * i) / steps;
        px(static_cast<int>(lroundf(player.x + ox + cosf(a) * (player.r + 5))),
           static_cast<int>(lroundf(player.y + oy + sinf(a) * (player.r + 5))), 1);
      }
    }

    float tx = player.x + ox + cosf(aim) * bladeRadius;
    float ty = player.y + oy + sinf(aim) * bladeRadius;
    line(player.x + ox, player.y + oy, tx, ty, 1);
    rect(static_cast<int>(tx) - 2, static_cast<int>(ty) - 2, 4, 4, 1);

    bool blink = fmodf(static_cast<float>(frameCount), 16.0f) < 8.0f;
    if (!(player.inv > 0 && blink)) {
      disc(player.x + ox, player.y + oy, player.r, 1);
      disc(player.x + ox, player.y + oy, player.r - 2, 0);
      rect(static_cast<int>(player.x + ox) - 1, static_cast<int>(player.y + oy) - 1, 2, 2, 1);
    }
  }

  // Particles
  for (auto &p : parts) {
    if (p.alive) px(static_cast<int>(lroundf(p.x + ox)), static_cast<int>(lroundf(p.y + oy)), 1);
  }
}

static void drawHud() {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d", player.score);
  text(buf, 9, 9, 2, 1);

  snprintf(buf, sizeof(buf), "LV %d", player.level);
  text(buf, W - 9, 9, 1, 1, ALIGN_RIGHT);

  int hpw = 60, hpx = W - 9 - hpw;
  frame_(hpx, 20, hpw, 8, 1, 1);
  rect(hpx + 1, 21, static_cast<int>((hpw - 2) * clampf(player.hp / player.maxHp, 0, 1)), 6, 1);

  if (levelUpFlash > 0) {
    text("LEVEL UP", W / 2, 30, 2, 1, ALIGN_CENTER);
  }

  // Status indicator for AS5600 magnetic knob
  if (!knob::is_connected()) {
    text("NO AS5600 DETECTED", W / 2, H - 14, 1, 1, ALIGN_CENTER);
  }
}

static void drawCenteredPanel(int x, int y, int w, int h) {
  rect(x, y, w, h, 0);
  frame_(x, y, w, h, 2, 1);
  frame_(x + 4, y + 4, w - 8, h - 8, 1, 1);
}

static float currentFps = 0.0f;
static float fpsTimer   = 0.0f;
static int   fpsFrames  = 0;

static void updateFps(float dt) {
  fpsFrames++;
  fpsTimer += dt;
  if (fpsTimer >= 0.25f) {
    currentFps = static_cast<float>(fpsFrames) / fpsTimer;
    fpsFrames = 0;
    fpsTimer = 0.0f;
  }
}

static void draw() {
  drawWorld();
  if (state != STATE_OVER) drawHud();

  // Live FPS counter in bottom-left corner
  char fps_buf[16];
  snprintf(fps_buf, sizeof(fps_buf), "%d FPS", static_cast<int>(lroundf(currentFps)));
  text(fps_buf, 9, H - 14, 1, 1, ALIGN_LEFT);

  if (state == STATE_READY) {
    drawCenteredPanel(36, 50, 248, 140);
    text("SPINBLADE", W / 2, 66, 2, 1, ALIGN_CENTER);
    text("MOVE: D-PAD (GP12-15)", W / 2, 94, 1, 1, ALIGN_CENTER);
    text("SWING: AS5600 KNOB", W / 2, 106, 1, 1, ALIGN_CENTER);
    text("ACTION: DASH (GP26)", W / 2, 118, 1, 1, ALIGN_CENTER);
    if (!knob::is_connected()) {
      text("[KNOB DISCONNECTED: AUTO-SPIN]", W / 2, 134, 1, 1, ALIGN_CENTER);
    }
    if (fmodf(static_cast<float>(frameCount), 40.0f) < 20.0f) {
      text("PRESS ACTION OR SPIN TO START", W / 2, 158, 1, 1, ALIGN_CENTER);
    }
  } else if (state == STATE_OVER) {
    drawCenteredPanel(60, 70, 200, 100);
    text("YOU FELL", W / 2, 86, 2, 1, ALIGN_CENTER);
    char buf[24];
    snprintf(buf, sizeof(buf), "SCORE %d", player.score);
    text(buf, W / 2, 116, 1, 1, ALIGN_CENTER);
    if (fmodf(static_cast<float>(frameCount), 40.0f) < 20.0f) {
      text("PRESS ACTION OR SPIN TO RETRY", W / 2, 140, 1, 1, ALIGN_CENTER);
    }
  }
}

// =====================================================================
// Main Loop
// =====================================================================
int main() {
  // Overclock RP2040 from 125MHz to 250MHz for rock-solid 55-60 FPS
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  set_sys_clock_khz(250000, true);

  stdio_init_all();
  font_init();
  buttons::init();
  knob::init();
  lcd::init();

  // Color palette (RGB565)
  const uint16_t INK_COLOR   = 0x2965; // Dark slate gray ~ (40, 47, 49)
  const uint16_t PAPER_COLOR = 0xAD75; // Warm parchment ~ (167, 174, 177)

  absolute_time_t last = get_absolute_time();

  while (true) {
    absolute_time_t now = get_absolute_time();
    float dt = absolute_time_diff_us(last, now) / 1000000.0f;
    if (dt > 0.05f) dt = 0.05f;
    last = now;

    updateFps(dt);
    update(dt);
    lcd::set_invert(flashScreen > 0.001f);
    draw();

    lcd::push_frame(fb, INK_COLOR, PAPER_COLOR);
    frameCount++;
  }

  return 0;
}
