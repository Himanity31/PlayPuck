// =====================================================================
// KURU VIDEO PLAYER -- High-Performance USB Streamer Firmware (RP2040)
//
// Features:
//   - Real-time video streaming from laptop over USB CDC
//   - Mode 1: Full-Color 16-bit with Real-Time Bilinear 2x Upscaling to 320x240!
//   - Mode 2: 1-Bit Retro Cinema (320x240 Bayer Dithered Noir/Sepia)
//   - Exclusive Core 1 SPI ownership (fixes white-screen SPI collision bug!)
//   - Strict header validation & overflow protection (fixes scrub desync bug!)
//   - Non-blocking USB control feedback (buttons & knob never stall video RX)
// =====================================================================

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <initializer_list>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/stdio_usb.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

extern "C" {
  int stdio_usb_in_chars(char *buf, int length);
}

// ---------------------------------------------------------------------
// Pin Map (KURU Handheld Console)
// ---------------------------------------------------------------------
#define PIN_SCK     2   // ILI9341 SCK
#define PIN_MOSI    3   // ILI9341 MOSI
#define PIN_CS      5   // ILI9341 CS
#define PIN_DC      6   // ILI9341 DC
#define PIN_RST     7   // ILI9341 RST
#define PIN_BL      8   // ILI9341 Backlight

#define PIN_SDA    10   // AS5600 SDA (I2C1)
#define PIN_SCL    11   // AS5600 SCL (I2C1)
#define AS5600_ADDR 0x36

#define BTN_UP     12   // D-Pad Up
#define BTN_DOWN   13   // D-Pad Down
#define BTN_LEFT   14   // D-Pad Left
#define BTN_RIGHT  15   // D-Pad Right
#define BTN_A      26   // Button A: Play / Pause
#define BTN_B      27   // Button B: Toggle Mode (Color <-> 1-Bit)
#define BTN_START  28   // Button Start
#define BTN_SELECT 29   // Button Select

static const uint32_t SPI_BAUD = 62500000; // 62.5 MHz SPI
static const int W = 320, H = 240;

// Frame Magic & Protocol Constants
static const uint32_t PROTOCOL_MAGIC = 0x5552554B; // "KURU" in little-endian
static const uint8_t  MODE_COLOR_160X120     = 0x01; // 38,400 bytes RGB565 (Legacy uncompressed)
static const uint8_t  MODE_MONO_320X240      = 0x02; // 9,600 bytes 1-bit packed
static const uint8_t  MODE_TURBO_160X120     = 0x04; // Turbo Delta 160x120 16-bit Color (60 FPS)
static const uint8_t  MODE_TURBO_GBA_240X160 = 0x05; // Turbo Delta 240x160 Native GBA 16-bit Color (60 FPS)

// Video Buffer sizes
static const uint32_t COLOR_FRAME_SIZE = 160 * 120 * 2; // 38,400 bytes
static const uint32_t GBA_FRAME_SIZE   = 240 * 160 * 2; // 76,800 bytes
static const uint32_t MONO_FRAME_SIZE  = 320 * 240 / 8; // 9,600 bytes
static const uint32_t MAX_FRAME_SIZE   = GBA_FRAME_SIZE;
static const uint32_t MAX_PAYLOAD_SIZE = 48 * 1024;     // 48 KB

// Fast single-instruction 50/50 RGB565/BGR565 pixel blender for bilinear upscaling
static inline uint16_t blend565(uint16_t a, uint16_t b) {
  return ((a & 0xF7DEu) >> 1) + ((b & 0xF7DEu) >> 1) + (a & b & 0x0821u);
}

// ---------------------------------------------------------------------
// Display Driver (ILI9341, Core 1 Exclusive DMA Streaming)
// ---------------------------------------------------------------------
namespace lcd {
  static inline void cs_low()  { gpio_put(PIN_CS, 0); }
  static inline void cs_high() { gpio_put(PIN_CS, 1); }
  static inline void dc_cmd()  { gpio_put(PIN_DC, 0); }
  static inline void dc_data() { gpio_put(PIN_DC, 1); }

  static int dma_tx = -1;
  static uint16_t dma_row[2][W]; // Ping-pong row buffers for DMA
  static uint16_t next_even_row[W];

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

    // Hardware reset
    gpio_put(PIN_RST, 1); sleep_ms(10);
    gpio_put(PIN_RST, 0); sleep_ms(20);
    gpio_put(PIN_RST, 1); sleep_ms(150);

    write_cmd(0x01); sleep_ms(150); // Software reset
    write_cmd(0x11); sleep_ms(150); // Exit Sleep

    cmd_args(0x3A, {0x55}); // 16-bit RGB565
    cmd_args(0x36, {0xA8}); // Landscape orientation
    write_cmd(0x20);        // Inversion OFF
    cmd_args(0x26, {0x01}); // Gamma curve 1

    write_cmd(0x29); sleep_ms(50);  // Display ON
  }

  static void set_window(int x0, int y0, int x1, int y1) {
    cmd_args(0x2A, {(uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)});
    cmd_args(0x2B, {(uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)});
  }
} // namespace lcd

// ---------------------------------------------------------------------
// Double Buffering & Display State
// ---------------------------------------------------------------------
static uint8_t frame_buffers[2][MAX_FRAME_SIZE + 64]; // Holds 160x120 or 240x160
static uint8_t compressed_rx_buf[MAX_PAYLOAD_SIZE];
static volatile int display_buf_idx = 0;
static volatile int write_buf_idx   = 1;
static volatile uint8_t current_mode = MODE_COLOR_160X120;
static volatile bool video_active    = false;
static bool border_cleared_mode5     = false;

// Noir / Sepia Colors for Mode 2 (matches 0xA8 BGR565 directly)
static const uint16_t INK_COLOR   = 0x18C3; // Deep charcoal noir
static const uint16_t PAPER_COLOR = 0xB6FD; // Warm parchment sepia in BGR565

// Expand 160 pixels into 320 horizontally-interpolated pixels
static inline void expand_row_bilinear_x(const uint16_t *row_src, uint16_t *dst320) {
  uint16_t prev = (row_src[0] >> 8) | (row_src[0] << 8);
  for (int x = 0; x < 159; x++) {
    uint16_t next = (row_src[x + 1] >> 8) | (row_src[x + 1] << 8);
    dst320[x * 2]     = prev;
    dst320[x * 2 + 1] = blend565(prev, next);
    prev = next;
  }
  dst320[318] = prev;
  dst320[319] = prev;
}

static void decode_delta_scanlines(const uint8_t *payload, uint32_t len, uint8_t *dst_buffer, int width, int height, bool is_keyframe) {
  int mask_bytes = (height + 7) / 8;
  if (len < static_cast<uint32_t>(mask_bytes)) return;

  const uint8_t *mask = payload;
  const uint8_t *src = payload + mask_bytes;
  const uint8_t *src_end = payload + len;

  uint16_t *frame16 = reinterpret_cast<uint16_t*>(dst_buffer);

  for (int y = 0; y < height; y++) {
    bool line_changed = (mask[y / 8] & (1 << (y % 8))) != 0;
    if (!line_changed) {
      continue;
    }

    uint16_t *line_dst = frame16 + (y * width);
    if (width == 240) {
      // Mode 5 (240x160 Native GBA): direct DMA halfword format
      for (int x = 0; x < width && (src + 1) < src_end; x++) {
        line_dst[x] = (static_cast<uint16_t>(src[0]) << 8) | src[1];
        src += 2;
      }
    } else {
      // Mode 4 (160x120): byte layout matching Mode 1 for expand_row_bilinear_x
      for (int x = 0; x < width && (src + 1) < src_end; x++) {
        line_dst[x] = (static_cast<uint16_t>(src[1]) << 8) | src[0];
        src += 2;
      }
    }
  }
}

static void process_completed_frame(uint8_t mode, uint8_t flags, uint32_t len) {
  if (mode == MODE_TURBO_GBA_240X160) {
    bool is_keyframe = (flags & 0x01) != 0;
    if (!is_keyframe) {
      memcpy(frame_buffers[write_buf_idx], frame_buffers[display_buf_idx], GBA_FRAME_SIZE);
    }
    decode_delta_scanlines(compressed_rx_buf, len, frame_buffers[write_buf_idx], 240, 160, is_keyframe);
    current_mode = mode;
    display_buf_idx = write_buf_idx;
    write_buf_idx ^= 1;
    video_active = true;
  } else if (mode == MODE_TURBO_160X120) {
    bool is_keyframe = (flags & 0x01) != 0;
    if (!is_keyframe) {
      memcpy(frame_buffers[write_buf_idx], frame_buffers[display_buf_idx], COLOR_FRAME_SIZE);
    }
    decode_delta_scanlines(compressed_rx_buf, len, frame_buffers[write_buf_idx], 160, 120, is_keyframe);
    current_mode = mode;
    display_buf_idx = write_buf_idx;
    write_buf_idx ^= 1;
    video_active = true;
  } else {
    current_mode = mode;
    display_buf_idx = write_buf_idx;
    write_buf_idx ^= 1;
    video_active = true;
  }
}

// ---------------------------------------------------------------------
// Core 1: Exclusive High-Speed Display Loop (Never Collides with Core 0!)
// ---------------------------------------------------------------------
void core1_display_runner() {
  while (true) {
    if (!video_active) {
      sleep_ms(5);
      continue;
    }

    int buf_to_render = display_buf_idx;
    uint8_t mode = current_mode;
    const uint8_t *src = frame_buffers[buf_to_render];

    if (mode == MODE_TURBO_GBA_240X160) {
      if (!border_cleared_mode5) {
        // Clear full 320x240 LCD once to deep pitch black for authentic bezel
        lcd::set_window(0, 0, W - 1, H - 1);
        lcd::cs_low();
        lcd::dc_cmd();
        uint8_t ram_cmd = 0x2C;
        spi_write_blocking(spi0, &ram_cmd, 1);
        lcd::dc_data();
        spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        uint16_t black = 0x0000;
        for (int i = 0; i < W * H; i++) {
          spi_write16_blocking(spi0, &black, 1);
        }
        lcd::cs_high();
        spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        border_cleared_mode5 = true;
      }

      // 1:1 Pixel-Perfect Native GBA (240x160 centered inside 320x240 LCD: X: 40..279, Y: 40..199)
      const uint16_t *src16 = reinterpret_cast<const uint16_t*>(src);
      lcd::set_window(40, 40, 279, 199);
      lcd::cs_low();
      lcd::dc_cmd();
      uint8_t ram_cmd = 0x2C;
      spi_write_blocking(spi0, &ram_cmd, 1);
      lcd::dc_data();
      spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

      // Single hardware DMA transfer of exactly 240 * 160 pixels (38,400 halfwords)
      dma_channel_transfer_from_buffer_now(lcd::dma_tx, src16, 240 * 160);
      dma_channel_wait_for_finish_blocking(lcd::dma_tx);

      while (spi_is_busy(spi0)) tight_loop_contents();
      lcd::cs_high();
      spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    } else if (mode == MODE_COLOR_160X120 || mode == MODE_TURBO_160X120) {
      border_cleared_mode5 = false;

      // 160x120 -> 320x240 with Real-Time Bilinear 2x Interpolation & Ping-Pong DMA!
      lcd::set_window(0, 0, W - 1, H - 1);
      lcd::cs_low();
      lcd::dc_cmd();
      uint8_t ram_cmd = 0x2C;
      spi_write_blocking(spi0, &ram_cmd, 1);
      lcd::dc_data();
      spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

      const uint16_t *src16 = reinterpret_cast<const uint16_t*>(src);

      // Prepare row 0
      expand_row_bilinear_x(src16, lcd::dma_row[0]);

      for (int y = 0; y < 120; y++) {
        // 1. Start DMA for even scanline (2*y) from dma_row[0]
        dma_channel_transfer_from_buffer_now(lcd::dma_tx, lcd::dma_row[0], W);

        // While DMA sends even scanline, CPU computes next even row (y+1) and blended odd scanline (2*y+1)!
        if (y + 1 < 120) {
          expand_row_bilinear_x(src16 + (y + 1) * 160, lcd::next_even_row);
          for (int x = 0; x < W; x++) {
            lcd::dma_row[1][x] = blend565(lcd::dma_row[0][x], lcd::next_even_row[x]);
          }
        } else {
          memcpy(lcd::dma_row[1], lcd::dma_row[0], W * sizeof(uint16_t));
        }

        dma_channel_wait_for_finish_blocking(lcd::dma_tx);

        // 2. Start DMA for interpolated odd scanline (2*y+1) from dma_row[1]
        dma_channel_transfer_from_buffer_now(lcd::dma_tx, lcd::dma_row[1], W);

        // Copy precomputed next_even_row into dma_row[0] for next iteration
        if (y + 1 < 120) {
          memcpy(lcd::dma_row[0], lcd::next_even_row, W * sizeof(uint16_t));
        }

        dma_channel_wait_for_finish_blocking(lcd::dma_tx);
      }

      while (spi_is_busy(spi0)) tight_loop_contents();
      lcd::cs_high();
      spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    } else if (mode == MODE_MONO_320X240) {
      border_cleared_mode5 = false;

      // 320x240 1-bit Bayer Dithered Noir / Sepia (9,600 bytes) with Ping-Pong DMA
      lcd::set_window(0, 0, W - 1, H - 1);
      lcd::cs_low();
      lcd::dc_cmd();
      uint8_t ram_cmd = 0x2C;
      spi_write_blocking(spi0, &ram_cmd, 1);
      lcd::dc_data();
      spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

      int buf_idx = 0;
      for (int y = 0; y < 240; y++) {
        const uint8_t *row_bytes = src + (y * 40);
        uint16_t *row_dst = lcd::dma_row[buf_idx];

        int pixel_idx = 0;
        for (int b = 0; b < 40; b++) {
          uint8_t byte_val = row_bytes[b];
          for (int bit = 7; bit >= 0; bit--) {
            row_dst[pixel_idx++] = (byte_val & (1 << bit)) ? INK_COLOR : PAPER_COLOR;
          }
        }

        dma_channel_transfer_from_buffer_now(lcd::dma_tx, row_dst, W);
        dma_channel_wait_for_finish_blocking(lcd::dma_tx);
        buf_idx ^= 1;
      }

      while (spi_is_busy(spi0)) tight_loop_contents();
      lcd::cs_high();
      spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    }
  }
}

// ---------------------------------------------------------------------
// Standby Splash Screen (Rendered ONCE on Core 0 BEFORE Core 1 launches)
// ---------------------------------------------------------------------
static void draw_rect(int x, int y, int w, int h, uint16_t col) {
  lcd::set_window(x, y, x + w - 1, y + h - 1);
  lcd::cs_low();
  lcd::dc_cmd();
  uint8_t cmd = 0x2C;
  spi_write_blocking(spi0, &cmd, 1);
  lcd::dc_data();
  spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  for (int i = 0; i < w * h; i++) {
    spi_write16_blocking(spi0, &col, 1);
  }
  lcd::cs_high();
  spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

static const uint8_t FONT5x7[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, // ' '
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
  {0x7F,0x20,0x18,0x20,0x7F}, // 'W'
  {0x63,0x14,0x08,0x14,0x63}, // 'X'
  {0x07,0x08,0x70,0x08,0x07}, // 'Y'
  {0x61,0x51,0x49,0x45,0x43}, // 'Z'
  {0x00,0x36,0x36,0x00,0x00}, // ':'
  {0x00,0x00,0x60,0x60,0x00}, // '.'
  {0x08,0x08,0x08,0x08,0x08}, // '-'
  {0x20,0x10,0x08,0x04,0x02}, // '/'
  {0x00,0x00,0x5F,0x00,0x00}  // '!'
};

static uint8_t get_font_byte(char c, int col) {
  if (c >= 'a' && c <= 'z') c -= 32;
  int idx = 0;
  if (c >= '0' && c <= '9') idx = c - '0' + 1;
  else if (c >= 'A' && c <= 'Z') idx = c - 'A' + 11;
  else if (c == ':') idx = 37;
  else if (c == '.') idx = 38;
  else if (c == '-') idx = 39;
  else if (c == '/') idx = 40;
  else if (c == '!') idx = 41;
  if (col >= 0 && col < 5) return FONT5x7[idx][col];
  return 0;
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t col) {
  while (*s) {
    char c = *s++;
    for (int col_i = 0; col_i < 5; col_i++) {
      uint8_t bits = get_font_byte(c, col_i);
      for (int row_i = 0; row_i < 7; row_i++) {
        if (bits & (1 << row_i)) {
          draw_rect(x + col_i * scale, y + row_i * scale, scale, scale, col);
        }
      }
    }
    x += 6 * scale;
  }
}

static void show_standby_screen() {
  draw_rect(0, 0, W, H, 0x18C3);

  draw_rect(10, 10, W - 20, 2, 0xB6FD);
  draw_rect(10, H - 12, W - 20, 2, 0xB6FD);
  draw_rect(10, 10, 2, H - 20, 0xB6FD);
  draw_rect(W - 12, 10, 2, H - 20, 0xB6FD);

  draw_text(48, 32, "KURU VIDEO PLAYER", 2, 0xB6FD);
  draw_text(48, 56, "BILINEAR HD STREAMER", 1, 0x07E0);

  draw_rect(130, 78, 60, 42, 0x2124);
  draw_rect(130, 78, 60, 2, 0xB6FD);
  draw_rect(130, 118, 60, 2, 0xB6FD);
  draw_rect(130, 78, 2, 42, 0xB6FD);
  draw_rect(188, 78, 2, 42, 0xB6FD);
  draw_text(142, 94, "USB IN", 1, 0xFFFF);

  draw_text(36, 134, "WAITING FOR USB STREAM...", 1, 0xFFFF);
  draw_text(20, 150, "MAC: play_on_kuru.py | ANDROID: KURU Cast", 1, 0xB6FD);

  draw_text(26, 180, "CONTROLS: D-PAD & A/B/START/SELECT", 1, 0x7BEF);
  draw_text(26, 194, "KNOB = SCRUB / ANALOG  |  BTN A = ACTION", 1, 0x7BEF);
  draw_text(26, 208, "DIRECT PHONE EMULATOR & MAC STREAMING", 1, 0x7BEF);
}

// ---------------------------------------------------------------------
// AS5600 Rotary Encoder & Buttons
// ---------------------------------------------------------------------
namespace knob {
  static void init() {
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);
  }

  static bool read_raw(uint16_t *out) {
    uint8_t reg = 0x0C;
    if (i2c_write_timeout_us(i2c1, AS5600_ADDR, &reg, 1, true, 1200) < 0) return false;
    uint8_t buf[2];
    if (i2c_read_timeout_us(i2c1, AS5600_ADDR, buf, 2, false, 1200) < 0) return false;
    *out = ((static_cast<uint16_t>(buf[0]) << 8) | buf[1]) & 0x0FFF;
    return true;
  }
} // namespace knob

namespace buttons {
  static void init() {
    for (int p : {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_START, BTN_SELECT}) {
      gpio_init(p);
      gpio_set_dir(p, GPIO_IN);
      gpio_pull_up(p);
    }
  }
  static inline bool down(int p) { return !gpio_get(p); }
} // namespace buttons

// ---------------------------------------------------------------------
// Main Reception Engine (Core 0)
// ---------------------------------------------------------------------
int main() {
  vreg_set_voltage(VREG_VOLTAGE_1_25);
  set_sys_clock_khz(250000, true);

  stdio_init_all();
  buttons::init();
  knob::init();
  lcd::init();

  // Draw standby screen BEFORE Core 1 takes exclusive ownership of SPI0!
  show_standby_screen();

  // Launch Core 1 dedicated video renderer (owns SPI0 100% from here on)
  multicore_launch_core1(core1_display_runner);

  enum RxState {
    WAIT_MAGIC,
    READ_HEADER,
    READ_PAYLOAD
  };

  RxState rx_state = WAIT_MAGIC;
  uint8_t magic_buf[4] = {};
  int magic_bytes = 0;

  uint8_t hdr_mode = 0;
  uint8_t hdr_flags = 0;
  uint32_t hdr_len = 0;
  uint8_t hdr_buf[6] = {};
  int hdr_bytes = 0;

  uint32_t payload_received = 0;
  uint32_t payload_target = 0;

  uint16_t last_knob_raw = 0;
  bool knob_initialized = false;

  bool prev_a = false, prev_b = false;
  bool prev_l = false, prev_r = false;
  bool prev_u = false, prev_d = false;
  bool prev_s = false, prev_e = false;

  uint32_t last_input_poll_ms = 0;

  while (true) {
    // 1. High-Speed Bulk Read from USB CDC
    if (rx_state == READ_PAYLOAD) {
      uint32_t needed = payload_target - payload_received;
      uint8_t *dest_ptr = (hdr_mode == MODE_TURBO_160X120 || hdr_mode == MODE_TURBO_GBA_240X160)
                            ? &compressed_rx_buf[payload_received]
                            : &frame_buffers[write_buf_idx][payload_received];
      int rd = stdio_usb_in_chars(reinterpret_cast<char*>(dest_ptr), needed);
      if (rd > 0) {
        payload_received += static_cast<uint32_t>(rd);
        if (payload_received >= payload_target) {
          process_completed_frame(hdr_mode, hdr_flags, payload_target);
          rx_state = WAIT_MAGIC;
          magic_bytes = 0;
        }
        continue;
      }
    } else {
      char temp_in[64];
      int available = stdio_usb_in_chars(temp_in, sizeof(temp_in));
      if (available > 0) {
        for (int i = 0; i < available; i++) {
          uint8_t b = static_cast<uint8_t>(temp_in[i]);

          if (rx_state == WAIT_MAGIC) {
            magic_buf[magic_bytes++] = b;
            if (magic_bytes == 4) {
              uint32_t m = (magic_buf[0]) | (magic_buf[1] << 8) | (magic_buf[2] << 16) | (magic_buf[3] << 24);
              if (m == PROTOCOL_MAGIC) {
                rx_state = READ_HEADER;
                hdr_bytes = 0;
              } else {
                magic_buf[0] = magic_buf[1];
                magic_buf[1] = magic_buf[2];
                magic_buf[2] = magic_buf[3];
                magic_bytes = 3;
              }
            }
          } else if (rx_state == READ_HEADER) {
            hdr_buf[hdr_bytes++] = b;
            if (hdr_bytes == 6) {
              hdr_mode  = hdr_buf[0];
              hdr_flags = hdr_buf[1];
              hdr_len   = (static_cast<uint32_t>(hdr_buf[2]) << 24) |
                          (static_cast<uint32_t>(hdr_buf[3]) << 16) |
                          (static_cast<uint32_t>(hdr_buf[4]) << 8)  |
                          static_cast<uint32_t>(hdr_buf[5]);

              // Strict validation for all 4 supported modes
              bool valid_color     = (hdr_mode == MODE_COLOR_160X120 && hdr_len == COLOR_FRAME_SIZE);
              bool valid_mono      = (hdr_mode == MODE_MONO_320X240  && hdr_len == MONO_FRAME_SIZE);
              bool valid_turbo_160 = (hdr_mode == MODE_TURBO_160X120 && hdr_len > 0 && hdr_len <= MAX_PAYLOAD_SIZE);
              bool valid_turbo_240 = (hdr_mode == MODE_TURBO_GBA_240X160 && hdr_len > 0 && hdr_len <= MAX_PAYLOAD_SIZE);

              if (valid_color || valid_mono || valid_turbo_160 || valid_turbo_240) {
                payload_target = hdr_len;
                payload_received = 0;
                rx_state = READ_PAYLOAD;
              } else {
                // False magic match inside pixel stream; resume searching for real header
                rx_state = WAIT_MAGIC;
                magic_bytes = 0;
              }
            }
          } else if (rx_state == READ_PAYLOAD) {
            uint8_t *dest_ptr = (hdr_mode == MODE_TURBO_160X120 || hdr_mode == MODE_TURBO_GBA_240X160)
                                  ? compressed_rx_buf
                                  : frame_buffers[write_buf_idx];
            dest_ptr[payload_received++] = b;
            if (payload_received >= payload_target) {
              process_completed_frame(hdr_mode, hdr_flags, payload_target);
              rx_state = WAIT_MAGIC;
              magic_bytes = 0;
            }
          }
        }
        continue;
      }
    }

    // 2. Poll Physical Controls & AS5600 Knob at 30 Hz (non-blocking, no fflush stall!)
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - last_input_poll_ms >= 33) {
      last_input_poll_ms = now_ms;

      bool cur_a = buttons::down(BTN_A);
      bool cur_b = buttons::down(BTN_B);
      bool cur_l = buttons::down(BTN_LEFT);
      bool cur_r = buttons::down(BTN_RIGHT);
      bool cur_u = buttons::down(BTN_UP);
      bool cur_d = buttons::down(BTN_DOWN);
      bool cur_s = buttons::down(BTN_START);
      bool cur_e = buttons::down(BTN_SELECT);

      // Button Press events (Uppercase)
      if (cur_a && !prev_a) putchar_raw('A');
      if (cur_b && !prev_b) putchar_raw('B');
      if (cur_l && !prev_l) putchar_raw('L');
      if (cur_r && !prev_r) putchar_raw('R');
      if (cur_u && !prev_u) putchar_raw('U');
      if (cur_d && !prev_d) putchar_raw('D');
      if (cur_s && !prev_s) putchar_raw('S');
      if (cur_e && !prev_e) putchar_raw('E');

      // Button Release events (Lowercase)
      if (!cur_a && prev_a) putchar_raw('a');
      if (!cur_b && prev_b) putchar_raw('b');
      if (!cur_l && prev_l) putchar_raw('l');
      if (!cur_r && prev_r) putchar_raw('r');
      if (!cur_u && prev_u) putchar_raw('u');
      if (!cur_d && prev_d) putchar_raw('d');
      if (!cur_s && prev_s) putchar_raw('s');
      if (!cur_e && prev_e) putchar_raw('e');

      prev_a = cur_a;
      prev_b = cur_b;
      prev_l = cur_l;
      prev_r = cur_r;
      prev_u = cur_u;
      prev_d = cur_d;
      prev_s = cur_s;
      prev_e = cur_e;

      uint16_t raw_knob;
      if (knob::read_raw(&raw_knob)) {
        if (knob_initialized) {
          int diff = static_cast<int>(raw_knob) - static_cast<int>(last_knob_raw);
          if (diff > 2048)  diff -= 4096;
          if (diff < -2048) diff += 4096;

          if (std::abs(diff) >= 24) {
            int ticks = diff / 24;
            if (ticks > 120) ticks = 120;
            if (ticks < -120) ticks = -120;
            putchar_raw('K');
            putchar_raw(static_cast<char>(ticks));
            last_knob_raw = raw_knob;
          }
        } else {
          last_knob_raw = raw_knob;
          knob_initialized = true;
        }
      }
    }
  }

  return 0;
}
