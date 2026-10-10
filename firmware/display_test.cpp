// =====================================================================
// ILI9341 Display Test for RP2040-Zero
//
// Purpose: Foolproof hardware verification test
// Tests full 320x240 landscape resolution, RGB color bars, border alignment,
// and screen fill to prove display functionality.
// =====================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "pico/stdlib.h"
#include "hardware/spi.h"

// Pin Map (RP2040-Zero)
#define PIN_SCK   2   // ILI9341 SCK  (SPI0 SCK)
#define PIN_MOSI  3   // ILI9341 SDI  (SPI0 TX)
#define PIN_CS    5   // ILI9341 CS
#define PIN_DC    6   // ILI9341 DC / RS
#define PIN_RST   7   // ILI9341 RESET
#define PIN_BL    8   // ILI9341 Backlight

static const uint32_t SPI_BAUD = 20 * 1000 * 1000; // 20 MHz (super clean, noise-free)

static const int W = 320;
static const int H = 240;

// Standard RGB565 Colors
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define YELLOW  0xFFE0
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define GRAY    0x7BEF

namespace lcd {
  static inline void cs_low()  { gpio_put(PIN_CS, 0); }
  static inline void cs_high() { gpio_put(PIN_CS, 1); }
  static inline void dc_cmd()  { gpio_put(PIN_DC, 0); }
  static inline void dc_data() { gpio_put(PIN_DC, 1); }

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

  static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    cmd_args(0x2A, {uint8_t(x0 >> 8), uint8_t(x0 & 0xFF), uint8_t(x1 >> 8), uint8_t(x1 & 0xFF)});
    cmd_args(0x2B, {uint8_t(y0 >> 8), uint8_t(y0 & 0xFF), uint8_t(y1 >> 8), uint8_t(y1 & 0xFF)});
  }

  static void init() {
    gpio_init(PIN_CS);  gpio_set_dir(PIN_CS, GPIO_OUT);  gpio_put(PIN_CS, 1);
    gpio_init(PIN_DC);  gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_init(PIN_RST); gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_init(PIN_BL);  gpio_set_dir(PIN_BL, GPIO_OUT);  gpio_put(PIN_BL, 1);

    spi_init(spi0, SPI_BAUD);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // 1. Hardware Reset
    gpio_put(PIN_RST, 1); sleep_ms(10);
    gpio_put(PIN_RST, 0); sleep_ms(20);
    gpio_put(PIN_RST, 1); sleep_ms(150);

    // 2. Software Reset
    write_cmd(0x01);
    sleep_ms(150);

    // 3. Sleep Out (CRITICAL: Must wake the chip BEFORE configuring registers!)
    write_cmd(0x11);
    sleep_ms(150);

    // 4. Pixel Format: 16-bit RGB565
    cmd_args(0x3A, {0x55});

    // 5. Memory Access Control (MADCTL): 320x240 Landscape (MV=1, MX=1, BGR=1) -> 0x68 un-mirrored
    cmd_args(0x36, {0x68});

    // 6. Display Inversion OFF
    write_cmd(0x20);

    // 7. Gamma Curve
    cmd_args(0x26, {0x01});

    // 8. Display ON
    write_cmd(0x29);
    sleep_ms(50);
  }

  static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x >= W || y >= H || w <= 0 || h <= 0) return;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;

    set_window(x, y, x + w - 1, y + h - 1);

    cs_low();
    dc_cmd();
    uint8_t cmd = 0x2C;
    spi_write_blocking(spi0, &cmd, 1);

    dc_data();
    uint16_t c_be = __builtin_bswap16(color);
    static uint16_t line_buf[320];
    for (int i = 0; i < w; i++) line_buf[i] = c_be;

    for (int row = 0; row < h; row++) {
      spi_write_blocking(spi0, reinterpret_cast<const uint8_t *>(line_buf), w * 2);
    }
    cs_high();
  }

  static void fill_screen(uint16_t color) {
    fill_rect(0, 0, W, H, color);
  }
}

// Simple 5x7 Font for Test Display
static const uint8_t FONT5x7[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, // ' '
  {0x00,0x00,0x5F,0x00,0x00}, // '!'
  {0x00,0x07,0x00,0x07,0x00}, // '"'
  {0x14,0x7F,0x14,0x7F,0x14}, // '#'
  {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
  {0x23,0x13,0x08,0x64,0x62}, // '%'
  {0x36,0x49,0x55,0x22,0x50}, // '&'
  {0x00,0x05,0x03,0x00,0x00}, // '''
  {0x00,0x1C,0x22,0x41,0x00}, // '('
  {0x00,0x41,0x22,0x1C,0x00}, // ')'
  {0x14,0x08,0x3E,0x08,0x14}, // '*'
  {0x08,0x08,0x3E,0x08,0x08}, // '+'
  {0x00,0x50,0x30,0x00,0x00}, // ','
  {0x08,0x08,0x08,0x08,0x08}, // '-'
  {0x00,0x60,0x60,0x00,0x00}, // '.'
  {0x20,0x10,0x08,0x04,0x02}, // '/'
  {0x3E,0x51,0x49,0x45,0x3E}, // '0'
  {0x00,0x42,0x7F,0x40,0x00}, // '1'
  {0x42,0x61,0x51,0x49,0x46}, // '2'
  {0x21,0x41,0x45,0x4B,0x31}, // '3'
  {0x18,0x14,0x12,0x7F,0x10}, // '4'
  {0x27,0x45,0x45,0x45,0x39}, // '5'
  {0x3C,0x4A,0x49,0x49,0x30}, // '6'
  {0x01,0x71,0x09,0x05,0x03}, // '7'
  {0x36,0x49,0x49,0x49,0x36}, // '8'
  {0x06,0x49,0x49,0x29,0x1E}, // '9'
  {0x00,0x36,0x36,0x00,0x00}, // ':'
  {0x00,0x56,0x36,0x00,0x00}, // ';'
  {0x08,0x14,0x22,0x41,0x00}, // '<'
  {0x14,0x14,0x14,0x14,0x14}, // '='
  {0x00,0x41,0x22,0x14,0x08}, // '>'
  {0x02,0x01,0x51,0x09,0x06}, // '?'
  {0x32,0x49,0x79,0x41,0x3E}, // '@'
  {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
  {0x7F,0x49,0x49,0x49,0x36}, // 'B'
  {0x3E,0x41,0x41,0x41,0x22}, // 'C'
  {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
  {0x7F,0x49,0x49,0x49,0x41}, // 'E'
  {0x7F,0x09,0x09,0x09,0x01}, // 'F'
  {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
  {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
  {0x00,0x41,0x7F,0x41,0x00}, // 'I'
  {0x20,0x40,0x41,0x3F,0x01}, // 'J'
  {0x7F,0x08,0x14,0x22,0x41}, // 'K'
  {0x7F,0x40,0x40,0x40,0x40}, // 'L'
  {0x7F,0x02,0x0C,0x02,0x7F}, // 'M'
  {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
  {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
  {0x7F,0x09,0x09,0x09,0x06}, // 'P'
  {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
  {0x7F,0x09,0x19,0x29,0x46}, // 'R'
  {0x46,0x49,0x49,0x49,0x31}, // 'S'
  {0x01,0x01,0x7F,0x01,0x01}, // 'T'
  {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
  {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
  {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
  {0x63,0x14,0x08,0x14,0x63}, // 'X'
  {0x07,0x08,0x70,0x08,0x07}, // 'Y'
  {0x61,0x51,0x49,0x45,0x43}  // 'Z'
};

static void draw_char(int x, int y, char c, uint16_t color, uint16_t bg, int scale = 2) {
  if (c >= 'a' && c <= 'z') c -= 32;
  if (c < ' ' || c > 'Z') c = ' ';
  int idx = c - ' ';
  for (int col = 0; col < 5; col++) {
    uint8_t line = FONT5x7[idx][col];
    for (int row = 0; row < 8; row++) {
      uint16_t pixelColor = (line & (1 << row)) ? color : bg;
      if (scale == 1) {
        lcd::fill_rect(x + col, y + row, 1, 1, pixelColor);
      } else {
        lcd::fill_rect(x + col * scale, y + row * scale, scale, scale, pixelColor);
      }
    }
  }
}

static void draw_string(int x, int y, const char *s, uint16_t color, uint16_t bg, int scale = 2) {
  while (*s) {
    draw_char(x, y, *s++, color, bg, scale);
    x += 6 * scale;
  }
}

int main() {
  stdio_init_all();
  lcd::init();

  // Test Pattern 1: Solid Colors Cycle to test every sub-pixel
  const uint16_t cycleColors[] = {RED, GREEN, BLUE, WHITE, BLACK};
  for (uint16_t c : cycleColors) {
    lcd::fill_screen(c);
    sleep_ms(300);
  }

  // Test Pattern 2: Persistent Diagnostic Landscape Test Screen
  lcd::fill_screen(BLACK);

  // 1. Draw outer 4-pixel border to verify all 320x240 edges
  lcd::fill_rect(0, 0, 320, 4, RED);           // Top edge
  lcd::fill_rect(0, 236, 320, 4, BLUE);        // Bottom edge
  lcd::fill_rect(0, 0, 4, 240, GREEN);         // Left edge
  lcd::fill_rect(316, 0, 4, 240, YELLOW);      // Right edge (Tests column 316..319!)

  // 2. Titles
  draw_string(20, 16, "RP2040 DISPLAY TEST", WHITE, BLACK, 3);
  draw_string(20, 46, "320x240 LANDSCAPE OK", GREEN, BLACK, 2);

  // 3. Color Bar Palette across the middle
  const uint16_t bars[] = {WHITE, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, GRAY};
  int barW = 320 / 8; // 40 pixels each
  for (int i = 0; i < 8; i++) {
    lcd::fill_rect(i * barW, 75, barW, 40, bars[i]);
  }

  // 4. Corner Markers
  draw_string(8, 8, "(0,0)", WHITE, BLACK, 1);
  draw_string(240, 8, "(319,0)", WHITE, BLACK, 1);
  draw_string(8, 224, "(0,239)", WHITE, BLACK, 1);
  draw_string(230, 224, "(319,239)", WHITE, BLACK, 1);

  // 5. Active Bouncing Box Animation
  int bx = 50, by = 130, bvx = 2, bvy = 2;
  const int bw = 24, bh = 24;

  while (true) {
    // Erase previous position
    lcd::fill_rect(bx, by, bw, bh, BLACK);

    bx += bvx;
    by += bvy;
    if (bx <= 10 || bx + bw >= 310) bvx = -bvx;
    if (by <= 120 || by + bh >= 220) bvy = -bvy;

    // Draw box at new position
    lcd::fill_rect(bx, by, bw, bh, CYAN);

    sleep_ms(20);
  }

  return 0;
}
