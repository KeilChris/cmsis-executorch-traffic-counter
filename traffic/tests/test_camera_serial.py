"""Host checks of the actual serialized capture and input-selection blocks.

Mocks verify LCD/capture ordering, timeouts, playback and the worker guard.
These tests do not validate the camera peripheral or image quality.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

TRAFFIC = Path(__file__).resolve().parents[1]

MOCK = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#define APP_HAS_CAMERA 1
#define APP_HAS_DISPLAY 1
#define APP_HAS_SDS 1
#define TRAFFIC_CAMERA_SERIAL 1
#define CAMERA_WIDTH 416
#define CAMERA_HEIGHT 416
#define CAMERA_QUARTER_TURNS 0
enum Source { kTestImage, kCamera, kPlayback };
enum { REC_PLAY_IDLE, REC_PLAY_RECORD, REC_PLAY_PLAYBACK };
enum { Wait, Release, Capture, Convert, Playback };
constexpr int kSize = 416;
constexpr uint32_t kInputBytes = 3, osWaitForever = UINT32_MAX;
static uint8_t g_slot[2][3];
static uint16_t raw[1];
static uint32_t g_convert_cycles, tick;
static const int g_display_idle = 123;
static bool lcd_busy, permit_held, fail_capture, playback_eof;
static std::vector<int> events;
static uint32_t cycles() { return tick++; }
static void osSemaphoreAcquire(int id, uint32_t timeout) {
    assert(id == g_display_idle && timeout == osWaitForever && !permit_held);
    events.push_back(Wait);
    lcd_busy = false; // Simulate waiting for the display worker to finish.
    permit_held = true;
}
static void osSemaphoreRelease(int id) {
    assert(id == g_display_idle && permit_held);
    events.push_back(Release);
    permit_held = false;
}
static const void* camera_frame(uint32_t timeout) {
    assert(timeout == 200 && !lcd_busy && !permit_held);
    events.push_back(Capture);
    return fail_capture ? nullptr : raw;
}
static void image_rgb565_to_input(const uint16_t* frame, int w, int h,
                                  uint8_t* dest, int size, int rotation) {
    assert(frame == raw && w == 416 && h == 416 && dest == g_slot[0]);
    assert(size == kSize && rotation == 0);
    events.push_back(Convert);
    dest[0] = 42;
}
static uint32_t camera_frame_count() { return 0; }
static uint32_t camera_error_count() { return 0; }
static int rec_play_read_input(uint8_t* dest, uint32_t bytes, uint32_t* ts) {
    assert(dest == g_slot[0] && bytes == kInputBytes);
    events.push_back(Playback);
    *ts = 101;
    return playback_eof ? 0 : 1;
}
static void setup(bool busy = false) {
    events.clear(); tick = 0; lcd_busy = busy;
    permit_held = fail_capture = playback_eof = false;
}
'''

TESTS = r'''
int main() {
    setup(true);
    assert(app_input(true, true, REC_PLAY_IDLE) == kCamera);
    assert((events == std::vector<int>{Wait, Release, Capture, Convert}));
    assert(g_slot[0][0] == 42 && g_convert_cycles == 1);

    setup(true);
    fail_capture = true;
    assert(app_input(true, true, REC_PLAY_RECORD) == -1);
    assert((events == std::vector<int>{Wait, Release, Capture}));
    assert(!permit_held); // No leaked permit on the continue/timeout path.
    events.clear(); fail_capture = false;
    assert(app_input(true, true, REC_PLAY_RECORD) == kCamera);
    assert((events == std::vector<int>{Wait, Release, Capture, Convert}));

    setup(); // A failed/disabled display must not use its null semaphore.
    assert(app_input(true, false, REC_PLAY_IDLE) == kCamera);
    assert((events == std::vector<int>{Capture, Convert}));

    setup(true); // Playback must not touch the sensor or wait for capture.
    assert(app_input(true, true, REC_PLAY_PLAYBACK) == kPlayback);
    assert((events == std::vector<int>{Playback}));
    assert(lcd_busy && !permit_held);

    setup(true);
    playback_eof = true;
    assert(app_input(true, true, REC_PLAY_PLAYBACK) == -1);
    assert((events == std::vector<int>{Playback}));

    setup(true);
    assert(app_input(false, true, REC_PLAY_IDLE) == kTestImage);
    assert(events.empty());
    puts("PASS: serial capture ordering, timeout recovery, display off, playback and fallback");
}
'''


class SerialCamera(unittest.TestCase):
    def test_actual_input_path(self):
        source = (TRAFFIC / "app_traffic.cpp").read_text()
        start = source.index("\nconst uint8_t* camera_input(") + 1
        capture = source[start:source.index("\n}\n", start) + 3]
        start = source.index("    // 1. The input.")
        selection = source[start:source.index("    const uint32_t t_input", start)]
        app = ("\nstatic int app_input(bool camera_on, bool display_on, int mode) {\n"
               "  uint8_t* g_input = g_slot[0]; uint32_t timeslot = 0;\n"
               "  for (int attempt = 0; attempt < 1; ++attempt) {\n" + selection +
               "    assert(input == g_slot[0]); (void)t0; return source;\n"
               "  }\n  return -1;\n}\n")
        with tempfile.TemporaryDirectory(prefix="camera-serial-") as tmp:
            executable = str(Path(tmp) / "test-camera")
            subprocess.run([os.environ.get("CXX", "clang++"), "-x", "c++", "-std=c++17",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-o", executable, "-"], input=MOCK + capture + app + TESTS,
                           text=True, check=True)
            subprocess.run([executable], check=True)

    def test_worker_is_only_created_in_original_mode(self):
        source = (TRAFFIC / "app_traffic.cpp").read_text()
        start = source.index("  const bool camera_on = camera_status == 0;")
        startup = source[start:source.index("\n#ifdef APP_HAS_SDS", start)]
        for serial in (0, 1):
            with self.subTest(serial=serial):
                result = subprocess.run(
                    [os.environ.get("CXX", "clang++"), "-E", "-P", "-x", "c++",
                     "-DAPP_HAS_CAMERA=1", "-DAPP_CAMERA_NUVOTON=1",
                     f"-DTRAFFIC_CAMERA_SERIAL={serial}", "-"],
                    input=startup, text=True, capture_output=True, check=True)
                self.assertEqual("osThreadNew(camera_thread" in result.stdout, serial == 0)


if __name__ == "__main__":
    unittest.main()
