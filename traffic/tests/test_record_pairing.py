"""Host regression of the real app recording blocks and rec_play write path.

No board access. Compile with sanitizers and mock stop/link events and SDS
buffer backpressure. A short test timeout exercises the production deadline.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

TRAFFIC = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index("\n" + signature) + 1
    return source[start:source.index("\n}\n", start) + 3]


MOCK = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "rec_play.h"
#define APP_HAS_SDS 1
#define SDS_FLAG_ALIVE 0x10000000U
#define SDS_FLAG_START 0x80000000U
#define SDS_STATE_STOP_REQ 4U
#define SDS_NO_SPACE (-4)
#define REC_PLAY_WAIT_MS 5U
using sdsId_t = int;
static sdsId_t in_id = 1, out_id = 2;
static uint32_t tick, sdsFlags, sdsState;
static unsigned input_calls, output_calls, input_busy, output_busy;
static bool stop_on_delay, lose_on_delay, stop_during_inference;
static int input_error, output_error;
static std::vector<uint32_t> recorded_inputs, recorded_outputs;
static uint32_t osKernelGetTickCount() { return tick; }
static void osDelay(uint32_t ticks) {
    tick += ticks;
    if (stop_on_delay) sdsFlags &= ~SDS_FLAG_START;
    if (lose_on_delay) sdsFlags &= ~SDS_FLAG_ALIVE;
}
static uint32_t cycles() { return tick; }
static int32_t sdsWrite(sdsId_t id, uint32_t ts, const void*, uint32_t size) {
    if (id == in_id) {
        ++input_calls;
        if (input_busy) { --input_busy; return SDS_NO_SPACE; }
        if (input_error) return input_error;
        recorded_inputs.push_back(ts);
    } else {
        assert(id == out_id);
        ++output_calls;
        if (output_busy) { --output_busy; return SDS_NO_SPACE; }
        if (output_error) return output_error;
        recorded_outputs.push_back(ts);
    }
    return static_cast<int32_t>(size);
}
static void setup() {
    tick = sdsState = input_calls = output_calls = input_busy = output_busy = 0;
    stop_on_delay = lose_on_delay = stop_during_inference = false;
    input_error = output_error = 0;
    sdsFlags = SDS_FLAG_ALIVE | SDS_FLAG_START;
    recorded_inputs.clear(); recorded_outputs.clear();
}
'''

TESTS = r'''
int main() {
    setup();
    app_frame(REC_PLAY_RECORD, 100);
    assert(recorded_inputs == std::vector<uint32_t>{100});
    assert(recorded_outputs == recorded_inputs);

    setup(); // The original 19-input / 20-output stop-boundary bug.
    input_busy = 1; stop_on_delay = true;
    app_frame(REC_PLAY_RECORD, 200);
    assert(input_calls == 1 && output_calls == 0);
    assert(recorded_inputs.empty() && recorded_outputs.empty());
    assert(sdsState == SDS_STATE_STOP_REQ);

    setup(); // Do not admit a new pair if Stop arrived before the input write.
    sdsFlags &= ~SDS_FLAG_START;
    app_frame(REC_PLAY_RECORD, 300);
    assert(input_calls == 0 && output_calls == 0);

    setup(); // Stop during inference still permits the accepted frame's result.
    stop_during_inference = true;
    app_frame(REC_PLAY_RECORD, 400);
    assert(recorded_outputs == recorded_inputs && recorded_outputs.size() == 1);

    setup(); // Finish a pair even when the output buffer needs to drain at Stop.
    output_busy = 2; stop_on_delay = true;
    app_frame(REC_PLAY_RECORD, 500);
    assert(output_calls == 3 && recorded_outputs == recorded_inputs);

    setup(); // Transient backpressure while recording is still active.
    input_busy = 2; output_busy = 1;
    app_frame(REC_PLAY_RECORD, 600);
    assert(input_calls == 3 && output_calls == 2);
    assert(recorded_outputs == recorded_inputs && recorded_outputs.size() == 1);

    setup(); // Error before accepting input must not produce an orphan result.
    input_error = -9;
    app_frame(REC_PLAY_RECORD, 700);
    assert(output_calls == 0 && sdsState == SDS_STATE_STOP_REQ);

    setup(); // A failed link cannot guarantee a pair, but must never hang.
    output_busy = 10; lose_on_delay = true;
    app_frame(REC_PLAY_RECORD, 800);
    assert(output_calls == 1 && tick == 1 && sdsState == SDS_STATE_STOP_REQ);

    setup(); // Bound a permanently full output buffer, also across tick wrap.
    output_busy = 100; stop_during_inference = true; tick = UINT32_MAX - 2;
    app_frame(REC_PLAY_RECORD, 900);
    assert(tick == 2 && output_calls == REC_PLAY_WAIT_MS + 1);
    assert(sdsState == SDS_STATE_STOP_REQ);

    setup(); // Playback outputs have a read input, not a newly recorded input.
    stop_during_inference = true;
    app_frame(REC_PLAY_PLAYBACK, 1000);
    assert(input_calls == 0 && recorded_outputs == std::vector<uint32_t>{1000});

    setup();
    app_frame(REC_PLAY_IDLE, 1100);
    assert(input_calls == 0 && output_calls == 0);
    puts("PASS: recording pairing, Stop/backpressure races, timeout, link loss, playback, idle");
}
'''


class RecordingPairing(unittest.TestCase):
    def test_actual_application_and_write_path(self):
        app = (TRAFFIC / "app_traffic.cpp").read_text()
        rec_play = (TRAFFIC / "rec_play.c").read_text()
        input_block = app[app.index("    // 2. Recording: the input."):app.index("    // 3. The detector.")]
        output_block = app[app.index("    // 5. The result, while recording or playing back."):app.index("    // 6. The panel:")]
        writer = "".join(function(rec_play, signature) for signature in (
            "static int32_t write_record(", "int32_t rec_play_write_input(", "int32_t rec_play_write_output("))
        frame = ("\nstatic void app_frame(int mode, uint32_t timeslot) {\n"
                 "  uint8_t input[3] = {}; const uint32_t kInputBytes = sizeof(input);\n"
                 "  int det = 0; uint32_t t0 = 0;\n" + input_block +
                 "  if (stop_during_inference) sdsFlags &= ~SDS_FLAG_START;\n" + output_block +
                 "  (void)t_sds;\n}\n")
        with tempfile.TemporaryDirectory(prefix="record-pairing-") as tmp:
            executable = str(Path(tmp) / "test-pairing")
            subprocess.run([os.environ.get("CXX", "clang++"), "-x", "c++", "-std=c++17",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I", str(TRAFFIC), "-o", executable, "-"],
                           input=MOCK + writer + frame + TESTS, text=True, check=True)
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
