# PlayPuck 🎮🕹️

> **A tactile rotary desk console powered by the Raspberry Pi RP2040, a 12-bit magnetic encoder, and high-framerate SPI graphics.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-RP2040-red.svg)](https://www.raspberrypi.com/products/rp2040/)
[![Sensor](https://img.shields.io/badge/Encoder-AS5600%20(12--bit)-blue.svg)]()
[![Display](https://img.shields.io/badge/Display-ILI9341%20(320x240)-green.svg)]()

PlayPuck is a custom pocket gaming console built around a smooth, continuous rotary dial. Unlike traditional handhelds with D-pads and thumbsticks, PlayPuck explores **rotary-first gameplay mechanics**—from angular velocity blade combat to high-precision safe-cracking puzzles with visual tactile simulation.

---

## 🕹️ Featured Games

### 1. SAFE CRACKER (Rogue-Lite Vault Breaker)
A tactical safe-cracking rogue-lite designed to simulate mechanical lockpicking **without audio or haptics**:
* **The Visual "Click" Engine:** 
  * **Acoustic Stethoscope Seismograph:** A real-time oscilloscope waveform at the bottom of the screen vibrates with harmonic resonance as you approach the true gate.
  * **Mechanical Detent Snap:** The reference needle physically bounces into the dial notch upon alignment.
  * **1-Frame Screen Shudder & Brass Sparks:** Optical confirmation of metal-on-metal alignment.
  * **Tumbler Cutaway Chamber:** Watch internal discs rotate and the overhead fence arm drop into the shear line.
* **100% Knob Playable:** Spin to dial, hold still for 0.75s to lock pins, browse and confirm rogue-lite relics without needing any physical buttons.
* **Rogue-Lite Upgrades:** 5 escalating bank vaults, false gates, heat/alarm timers, stethoscope durability, and draftable relics (Resonance Coil, Signal Filter, Magnetic Brake, etc.).

### 2. SPINBLADE (Orbital Action Defender)
A fast-paced arena action game running bare-metal on the RP2040:
* **Angular-Velocity Damage:** The faster you whip the physical dial, the more kinetic damage your blade deals to swarming grunts and brutes.
* **1-Bit Bayer Dithered Rendering:** Custom streaming engine delivering rock-solid 50+ FPS over overclocked 62.5 MHz SPI with DMA.

### 3. DISPLAY & HARDWARE TEST
A standalone diagnostic utility to verify ILI9341 color channels, RGB alignment, and SPI signal integrity.

---

## ⚡ Quickstart: Flash & Play in 30 Seconds

You don't need to install any compilers or toolchains to play. Pre-compiled UF2 binaries are included in the [`binaries/`](binaries/) folder:

1. Hold down the **BOOT** button on your RP2040-Zero.
2. Connect the USB-C cable to your computer (or tap RESET while holding BOOT).
3. Release the **BOOT** button. The board mounts as a drive named `RPI-RP2`.
4. Drag and drop any `.uf2` file onto the drive:
   * [`binaries/safecracker.uf2`](binaries/safecracker.uf2) — *Safe Dial Rogue-Lite*
   * [`binaries/spinblade.uf2`](binaries/spinblade.uf2) — *Orbital Action Defender*
5. The console reboots automatically into the game!

---

## 🔌 Hardware Pinout (RP2040-Zero)

All signals use the **outer header pins** of the Waveshare RP2040-Zero for clean breadboard and custom PCB integration:

| Component | Signal | RP2040-Zero Pin | Description |
| :--- | :--- | :--- | :--- |
| **ILI9341 Display** | VCC | 3V3 / 5V | Power |
| | GND | GND | Ground |
| | CS | **GP5** | Chip Select |
| | RESET | **GP7** | Reset |
| | DC / RS | **GP6** | Data / Command |
| | SDI / MOSI | **GP3** | SPI0 TX |
| | SCK / CLK | **GP2** | SPI0 SCK (62.5 MHz) |
| | LED / BL | **GP8** or 3V3 | Backlight |
| **AS5600 Encoder** | VCC | 3V3 | Power |
| | GND | GND | Ground |
| | SDA | **GP10** | I2C1 SDA (400 kHz) |
| | SCL | **GP11** | I2C1 SCL (400 kHz) |
| | DIR | GND | Direction (Clockwise) |
| **Optional Buttons** | D-Pad Up | **GP12** | Active-low (internal pull-up) |
| | D-Pad Down | **GP13** | Active-low (internal pull-up) |
| | D-Pad Left | **GP14** | Active-low (internal pull-up) |
| | D-Pad Right | **GP15** | Active-low (internal pull-up) |
| | Action / Dash | **GP26** | Active-low (internal pull-up) |

---

## 🛠️ Bill of Materials (BOM)

* **Microcontroller:** Waveshare RP2040-Zero (or standard Raspberry Pi Pico)
* **Rotary Sensor:** AS5600 12-bit Contactless Magnetic Rotary Encoder (I2C)
* **Magnet:** $\varnothing 6\text{ mm} \times 2.5\text{ mm}$ Diametric Neodymium Magnet (placed 1–2 mm above sensor)
* **Display:** 2.4" or 2.8" ILI9341 SPI TFT LCD ($320 \times 240$, 4-wire SPI)
* **Enclosure:** 3D printed enclosure and knob dial (files in [`cad/`](cad/))

---

## 📁 Repository Structure

```text
PlayPuck/
├── .gitignore
├── LICENSE                   # MIT License
├── README.md                 # Project Documentation
│
├── binaries/                 # Ready-to-flash .uf2 files
│   ├── safecracker.uf2
│   ├── spinblade.uf2
│   └── display_test.uf2
│
├── firmware/                 # C++ Source Code (Pico SDK)
│   ├── CMakeLists.txt
│   ├── pico_sdk_import.cmake
│   ├── safecracker.cpp       # Safe Cracker Rogue-Lite
│   ├── spinblade.cpp         # Spinblade Action Defender
│   └── display_test.cpp      # Hardware Diagnostic Tool
│
├── cad/                      # 3D Mechanical Files
│   ├── Knob_w_as5600.step    # Production STEP model with AS5600 mount
│   └── console_demo.blend    # Blender 3D console model & renders
│
└── media/                    # Gameplay and hardware demonstration
    └── playpuck_demo.mp4     # Video demonstration
```

---

## 💻 Building from Source

### Prerequisites
* [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (v1.5.0 or later)
* `arm-none-eabi-gcc` toolchain
* `cmake` and `make` (or `ninja`)

### Build Instructions

```bash
cd firmware
mkdir build && cd build

# Configure CMake (fetches Pico SDK automatically if not in environment)
cmake -DPICO_SDK_FETCH_FROM_GIT=on ..

# Build all targets:
make -j4
```

This compiles:
* `safecracker.uf2`
* `spinblade.uf2`
* `display_test.uf2`

---

## 📜 License

This project is open-source under the [MIT License](LICENSE). Feel free to build your own PlayPuck, modify the hardware, and create new rotary games!
