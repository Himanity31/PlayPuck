#!/usr/bin/env python3
"""
KURU Video Player - High-Performance Cinema USB Streamer for Mac
================================================================
Streams any video file (.mkv, .mp4, .mov, .avi, etc.) directly to the KURU
handheld console over USB CDC with:
  - True-Color 24-bit->16-bit Ordered Bayer Dithering (zero color banding)
  - Cinema Tone-Mapping (Shadow Gamma lift + Saturation boost + Unsharp Mask)
  - Correct Big-Endian BGR565 hardware pixel packing for the KURU ILI9341 LCD
  - Smart Cinema Aspect-Ratio Crop (16:10 Cinema / 4:3 Fullscreen / Original)
  - Anti-Desync Payload Sanitization & Keep-Alive (prevents white-screen bugs!)
"""

import sys
import os
import time
import argparse
import threading
import glob
import cv2
import numpy as np
import serial
import serial.tools.list_ports

# Protocol Constants
MAGIC = b"KURU"    # 0x5552554B
MODE_COLOR = 0x01  # 160x120 16-bit Color (38,400 bytes)
MODE_MONO  = 0x02  # 320x240 1-bit Bayer Dithered (9,600 bytes)

# ---------------------------------------------------------------------
# Precomputed Color Grading & Dithering Tables (Zero CPU Overhead)
# ---------------------------------------------------------------------
_x = np.arange(256, dtype=np.float32) / 255.0
_y = np.power(_x, 0.80)
_y = np.clip((_y - 0.5) * 1.14 + 0.52, 0.0, 1.0)
GAMMA_CONTRAST_LUT = (_y * 255.0).astype(np.uint8)

BAYER_4X4 = np.array([
    [ 0,  8,  2, 10],
    [12,  4, 14,  6],
    [ 3, 11,  1,  9],
    [15,  7, 13,  5]
], dtype=np.uint8) * 16
BAYER_GRID_MONO = np.tile(BAYER_4X4, (240 // 4, 320 // 4))

_B4_16 = np.array([
    [ 0,  8,  2, 10],
    [12,  4, 14,  6],
    [ 3, 11,  1,  9],
    [15,  7, 13,  5]
], dtype=np.uint16)
DITHER_RB = np.tile(_B4_16 >> 1, (120 // 4, 160 // 4))  # 0..7 dither for 5-bit R & B
DITHER_G  = np.tile(_B4_16 >> 2, (120 // 4, 160 // 4))  # 0..3 dither for 6-bit G

COLOR_PROFILES = [
    "BGR565-BE (Default KURU)",
    "RGB565-BE",
    "BGR565-LE",
    "RGB565-LE",
]

ASPECT_MODES = [
    "CINEMA 16:10 (Smart Crop)",
    "FULLSCREEN 4:3 (Fill)",
    "ORIGINAL (Full Letterbox)",
]


def find_kuru_port():
    """Auto-detect RP2040 KURU serial port on Mac."""
    ports = serial.tools.list_ports.comports()
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        dev = p.device
        if "usbmodem" in dev or "pico" in desc or "rp2040" in desc or "2e8a" in hwid:
            return dev

    modems = glob.glob("/dev/cu.usbmodem*")
    if modems:
        return modems[0]
    return None


def crop_and_resize(frame, target_w, target_h, aspect_mode=0):
    """Crop and resize frame according to selected aspect mode."""
    h, w = frame.shape[:2]

    if aspect_mode == 1:
        target_ratio = target_w / target_h
        cur_ratio = w / h
        if cur_ratio > target_ratio:
            new_w = int(h * target_ratio)
            x0 = (w - new_w) // 2
            frame = frame[:, x0:x0 + new_w]
        else:
            new_h = int(w / target_ratio)
            y0 = (h - new_h) // 2
            frame = frame[y0:y0 + new_h, :]
        return cv2.resize(frame, (target_w, target_h), interpolation=cv2.INTER_AREA)

    elif aspect_mode == 0:
        target_ratio = 1.65
        cur_ratio = w / h
        if cur_ratio > target_ratio:
            new_w = int(h * target_ratio)
            x0 = (w - new_w) // 2
            frame = frame[:, x0:x0 + new_w]
            h, w = frame.shape[:2]

    scale = min(target_w / w, target_h / h)
    new_w = max(1, int(w * scale))
    new_h = max(1, int(h * scale))
    resized = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_AREA)

    if len(frame.shape) == 3:
        canvas = np.zeros((target_h, target_w, 3), dtype=np.uint8)
    else:
        canvas = np.zeros((target_h, target_w), dtype=np.uint8)

    y_off = (target_h - new_h) // 2
    x_off = (target_w - new_w) // 2
    canvas[y_off:y_off + new_h, x_off:x_off + new_w] = resized
    return canvas


def enhance_color_frame(bgr_160x120):
    """Apply crispness sharpening, saturation boost, and shadow-lift gamma LUT."""
    blur = cv2.GaussianBlur(bgr_160x120, (0, 0), 0.9)
    sharp = cv2.addWeighted(bgr_160x120, 1.35, blur, -0.35, 0)

    hsv = cv2.cvtColor(sharp, cv2.COLOR_BGR2HSV)
    s = hsv[:, :, 1].astype(np.uint16)
    hsv[:, :, 1] = np.minimum((s * 128) >> 7, 255).astype(np.uint8)
    vibrant = cv2.cvtColor(hsv, cv2.COLOR_HSV2BGR)

    return cv2.LUT(vibrant, GAMMA_CONTRAST_LUT)


def encode_frame_color(bgr_frame, aspect_mode=0, color_profile=0):
    """Convert BGR frame to dithered 160x120 16-bit color buffer (38,400 bytes)."""
    fit = crop_and_resize(bgr_frame, 160, 120, aspect_mode)
    graded = enhance_color_frame(fit)

    b8 = graded[:, :, 0].astype(np.uint16)
    g8 = graded[:, :, 1].astype(np.uint16)
    r8 = graded[:, :, 2].astype(np.uint16)

    b5 = np.minimum(b8 + DITHER_RB, 255) >> 3
    g6 = np.minimum(g8 + DITHER_G,  255) >> 2
    r5 = np.minimum(r8 + DITHER_RB, 255) >> 3

    if color_profile == 0:
        pix16 = ((b5 << 11) | (g6 << 5) | r5).astype('>u2')
    elif color_profile == 1:
        pix16 = ((r5 << 11) | (g6 << 5) | b5).astype('>u2')
    elif color_profile == 2:
        pix16 = ((b5 << 11) | (g6 << 5) | r5).astype('<u2')
    else:
        pix16 = ((r5 << 11) | (g6 << 5) | b5).astype('<u2')

    # Sanitize any accidental b"KURU" sequence inside pixel payload so magic header is 100% unique!
    return pix16.tobytes().replace(b"KURU", b"KURT")


_CLAHE = cv2.createCLAHE(clipLimit=2.5, tileGridSize=(8, 8))

def encode_frame_mono(bgr_frame, aspect_mode=0):
    """Convert BGR frame to 320x240 1-bit Bayer dithered byte buffer (9,600 bytes)."""
    fit = crop_and_resize(bgr_frame, 320, 240, aspect_mode)
    gray = cv2.cvtColor(fit, cv2.COLOR_BGR2GRAY)
    enhanced = _CLAHE.apply(gray)
    dithered = (enhanced < BAYER_GRID_MONO)
    return np.packbits(dithered).tobytes().replace(b"KURU", b"KURT")


class KuruPlayer:
    def __init__(self, port, video_path, initial_mode=MODE_COLOR, max_fps=30.0, start_sec=180.0):
        self.port_name = port
        self.video_path = video_path
        self.mode = initial_mode
        self.max_fps = max_fps
        self.start_sec = start_sec
        self.aspect_mode = 0
        self.color_profile = 0
        self.is_paused = False
        self.running = True
        self.speed_mult = 1.0
        self.seek_offset_sec = 0.0
        self.ser = None

    def poll_kuru_controls(self):
        """Read and coalesce all pending button & knob commands between frames."""
        if not self.ser or not self.ser.is_open:
            return
        try:
            avail = self.ser.in_waiting
            if avail <= 0:
                return
            data = self.ser.read(avail)
            i = 0
            n = len(data)
            while i < n:
                ch = data[i]
                i += 1
                if ch == ord('A'):
                    self.is_paused = not self.is_paused
                elif ch == ord('B'):
                    self.mode = MODE_MONO if self.mode == MODE_COLOR else MODE_COLOR
                elif ch == ord('L'):
                    self.seek_offset_sec -= 10.0
                elif ch == ord('R'):
                    self.seek_offset_sec += 10.0
                elif ch == ord('U'):
                    self.color_profile = (self.color_profile + 1) % len(COLOR_PROFILES)
                elif ch == ord('D'):
                    self.aspect_mode = (self.aspect_mode + 1) % len(ASPECT_MODES)
                elif ch == ord('K'):
                    if i < n:
                        delta = int.from_bytes(data[i:i+1], byteorder='big', signed=True)
                        i += 1
                    else:
                        delta_b = self.ser.read(1)
                        delta = int.from_bytes(delta_b, byteorder='big', signed=True) if delta_b else 0
                    self.seek_offset_sec += delta * 1.5
        except Exception:
            pass

    def send_packet(self, header, payload):
        """Send complete frame packet atomically, auto-reconnecting if unplugged."""
        try:
            self.ser.write(header + payload)
            return True
        except Exception:
            print("\n[KURU disconnected or reflashing... waiting for USB reconnect...]")
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            while self.running:
                new_port = find_kuru_port()
                if new_port:
                    try:
                        self.ser = serial.Serial(new_port, baudrate=2000000, timeout=1.0)
                        self.port_name = new_port
                        print(f"[Reconnected to KURU on {new_port}!]")
                        return False
                    except Exception:
                        pass
                time.sleep(0.5)
            return False

    def run(self):
        if self.video_path == "0" or self.video_path.isdigit():
            cap = cv2.VideoCapture(int(self.video_path))
            is_camera = True
        else:
            if not os.path.isfile(self.video_path):
                print(f"Error: Video file '{self.video_path}' not found!")
                return
            cap = cv2.VideoCapture(self.video_path)
            is_camera = False

        if not cap.isOpened():
            print(f"Error: Could not open video '{self.video_path}'!")
            return

        total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        self.src_fps = cap.get(cv2.CAP_PROP_FPS) or 24.0

        cur_frame_idx = 0
        if not is_camera and self.start_sec > 0:
            cap.set(cv2.CAP_PROP_POS_MSEC, self.start_sec * 1000.0)
            cur_frame_idx = int(cap.get(cv2.CAP_PROP_POS_FRAMES))

        print(f"\n Connecting to KURU on {self.port_name}...")
        try:
            self.ser = serial.Serial(self.port_name, baudrate=2000000, timeout=1.0)
            self.ser.reset_input_buffer()
            self.ser.reset_output_buffer()
        except Exception as e:
            print(f"Failed to open serial port {self.port_name}: {e}")
            return

        print("\n" + "=" * 68)
        print("  KURU HANDHELD CINEMA STREAMER -- ZERO-GLITCH EDITION")
        print("=" * 68)
        print(f"  Target:     {self.port_name}")
        print(f"  Source:     {os.path.basename(self.video_path)} ({self.src_fps:.2f} FPS)")
        print("  Live KURU Controls:")
        print("    - Button A:    Play / Pause")
        print("    - Button B:    Toggle Full-Color <-> 1-Bit Noir")
        print("    - Knob:        Scrub / Seek Forward & Backward")
        print("    - D-Pad L/R:   Skip -10s / +10s")
        print("    - D-Pad Down:  Cycle Aspect Ratio (Cinema 16:10 / Full 4:3 / Raw)")
        print("    - D-Pad Up:    Cycle Color Format (BGR565-BE / RGB565-BE / etc.)")
        print("=" * 68 + "\n")

        fps_counter = 0
        fps_timer = time.time()
        live_fps = 0.0
        stream_start_wall = time.time()
        stream_start_frame = cur_frame_idx
        last_frame = None
        last_sent_wall = 0.0

        try:
            while cap.isOpened() and self.running:
                # Poll and batch all button/knob events strictly between frames!
                self.poll_kuru_controls()

                # Coalesced seek (executes once cleanly instead of stalling >3.5s)
                if self.seek_offset_sec != 0.0 and not is_camera:
                    cur_msec = cap.get(cv2.CAP_PROP_POS_MSEC)
                    max_msec = (total_frames / self.src_fps) * 1000.0 if total_frames > 0 else 99999999.0
                    new_msec = max(0.0, min(max_msec - 1000.0, cur_msec + self.seek_offset_sec * 1000.0))
                    self.seek_offset_sec = 0.0

                    # Send keep-alive frame BEFORE slow MKV seek so KURU never hits 3.5s standby timeout!
                    if last_frame is not None:
                        active_mode = self.mode
                        payload = encode_frame_color(last_frame, self.aspect_mode, self.color_profile) if active_mode == MODE_COLOR else encode_frame_mono(last_frame, self.aspect_mode)
                        header = bytearray(MAGIC) + bytearray([active_mode, 0x00]) + len(payload).to_bytes(4, 'big')
                        self.send_packet(header, payload)

                    cap.set(cv2.CAP_PROP_POS_MSEC, new_msec)
                    cur_frame_idx = int(cap.get(cv2.CAP_PROP_POS_FRAMES))
                    stream_start_wall = time.time()
                    stream_start_frame = cur_frame_idx

                if self.is_paused:
                    stream_start_wall = time.time()
                    stream_start_frame = cur_frame_idx
                    # Re-send paused frame every 0.8s so KURU never hits 3.5s standby timeout
                    if last_frame is not None and (time.time() - last_sent_wall >= 0.8):
                        active_mode = self.mode
                        payload = encode_frame_color(last_frame, self.aspect_mode, self.color_profile) if active_mode == MODE_COLOR else encode_frame_mono(last_frame, self.aspect_mode)
                        header = bytearray(MAGIC) + bytearray([active_mode, 0x00]) + len(payload).to_bytes(4, 'big')
                        self.send_packet(header, payload)
                        last_sent_wall = time.time()
                    time.sleep(0.03)
                    continue

                # Frame-skipping to stay locked to real-time movie playback
                if not is_camera:
                    expected_frame = stream_start_frame + int((time.time() - stream_start_wall) * self.src_fps * self.speed_mult)
                    frames_behind = expected_frame - cur_frame_idx
                    if frames_behind > 1 and frames_behind < 30:
                        for _ in range(min(frames_behind - 1, 3)):
                            cap.grab()
                            cur_frame_idx += 1

                ret, frame = cap.read()
                if not ret:
                    if not is_camera:
                        cap.set(cv2.CAP_PROP_POS_MSEC, 0)
                        cur_frame_idx = 0
                        stream_start_wall = time.time()
                        stream_start_frame = 0
                        continue
                    else:
                        break
                cur_frame_idx += 1
                last_frame = frame

                active_mode = self.mode
                if active_mode == MODE_COLOR:
                    payload = encode_frame_color(frame, self.aspect_mode, self.color_profile)
                else:
                    payload = encode_frame_mono(frame, self.aspect_mode)

                header = bytearray(MAGIC)
                header.append(active_mode)
                header.append(0x00)
                header.extend(len(payload).to_bytes(4, byteorder='big'))

                if not self.send_packet(header, payload):
                    stream_start_wall = time.time()
                    stream_start_frame = cur_frame_idx
                    continue
                last_sent_wall = time.time()

                fps_counter += 1
                now = time.time()
                if now - fps_timer >= 1.0:
                    live_fps = fps_counter / (now - fps_timer)
                    fps_counter = 0
                    fps_timer = now

                mode_name = COLOR_PROFILES[self.color_profile] if active_mode == MODE_COLOR else "1-BIT NOIR"
                aspect_name = ASPECT_MODES[self.aspect_mode].split()[0]
                if not is_camera and total_frames > 0:
                    sec_cur = int(cur_frame_idx / self.src_fps)
                    sec_tot = int(total_frames / self.src_fps)
                    t_cur = f"{sec_cur // 60:02d}:{sec_cur % 60:02d}"
                    t_tot = f"{sec_tot // 60:02d}:{sec_tot % 60:02d}"
                    pct = int((cur_frame_idx / total_frames) * 100)
                    progress_info = f"[{t_cur}/{t_tot}] ({pct:2d}%)"
                else:
                    progress_info = f"[Frame {cur_frame_idx}]"

                sys.stdout.write(
                    f"\r\033[K> {mode_name} | {aspect_name} | {live_fps:4.1f} FPS | {progress_info} "
                )
                sys.stdout.flush()

                if not is_camera:
                    target_wall = stream_start_wall + ((cur_frame_idx - stream_start_frame) / (self.src_fps * self.speed_mult))
                    wait = target_wall - time.time()
                    if wait > 0:
                        time.sleep(wait)

        except KeyboardInterrupt:
            print("\nPlayback stopped by user.")
        finally:
            self.running = False
            cap.release()
            if self.ser and self.ser.is_open:
                self.ser.close()
            print("\nClosed video stream.")


def main():
    parser = argparse.ArgumentParser(description="Stream video to KURU Handheld Console over USB")
    parser.add_argument("video", nargs="?", default="", help="Path to video file (.mkv, .mp4, etc.) or '0' for webcam")
    parser.add_argument("--port", "-p", default="", help="Serial port (default: auto-detect)")
    parser.add_argument("--mono", "-m", action="store_true", help="Start in 1-Bit Retro Cinema mode")
    parser.add_argument("--fps", type=float, default=30.0, help="Maximum target FPS")
    parser.add_argument("--start", "-s", type=float, default=0.0, help="Start time in seconds (default: 0s)")
    args = parser.parse_args()

    if not args.video:
        print("Usage: python3 play_on_kuru.py <video_file> [--mono] [--start 180]")
        sys.exit(1)

    port = args.port or find_kuru_port()
    if not port:
        print("Error: Could not find KURU USB device.")
        sys.exit(1)

    initial_mode = MODE_MONO if args.mono else MODE_COLOR
    player = KuruPlayer(port, args.video, initial_mode=initial_mode, max_fps=args.fps, start_sec=args.start)
    player.run()


if __name__ == "__main__":
    main()
