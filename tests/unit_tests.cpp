// unit_tests.cpp - Automated tests for the parts of the plug-in that need no sound card.
//
// Run them with:  ctest -C Release --output-on-failure
//
// The tests use a tiny CHECK macro instead of a test framework, so building them
// downloads nothing. The program exits with a non-zero code if any check fails.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "position_tracker.h"
#include "sample_convert.h"
#include "settings.h"
#include "spsc_ring.h"
#include "stream_control.h"

static int g_failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static bool Near(double a, double b) { return std::fabs(a - b) < 0.0001; }

// --- Ring buffer ------------------------------------------------------------

static void TestRingStartsEmpty() {
    SpscRing ring;
    ring.allocate(8);
    CHECK(ring.capacity() == 8);
    CHECK(ring.freeBytes() == 8);
    CHECK(ring.pendingBytes() == 0);
}

static void TestRingStopsWhenFull() {
    SpscRing ring;
    ring.allocate(4);
    const unsigned char data[6] = {1, 2, 3, 4, 5, 6};
    CHECK(ring.write(data, 6) == 4);
    CHECK(ring.freeBytes() == 0);
    CHECK(ring.write(data, 1) == 0);

    unsigned char out[6] = {0};
    CHECK(ring.read(out, 6) == 4);
    CHECK(memcmp(out, data, 4) == 0);
    CHECK(ring.read(out, 1) == 0);
}

static void TestRingWrapsAround() {
    SpscRing ring;
    ring.allocate(5);
    unsigned char out[5];

    // Repeating this moves the positions past the end of the storage many times.
    for (int round = 0; round < 50; ++round) {
        const unsigned char data[3] = {(unsigned char)round, (unsigned char)(round + 1), (unsigned char)(round + 2)};
        CHECK(ring.write(data, 3) == 3);
        CHECK(ring.pendingBytes() == 3);
        CHECK(ring.read(out, 3) == 3);
        CHECK(memcmp(out, data, 3) == 0);
    }
}

static void TestRingDiscardDropsOnlyOldAudio() {
    SpscRing ring;
    ring.allocate(8);
    const unsigned char oldAudio[3] = {1, 2, 3};
    const unsigned char newAudio[2] = {8, 9};

    ring.write(oldAudio, 3);
    ring.requestDiscard();
    CHECK(ring.pendingBytes() == 0);
    ring.write(newAudio, 2);
    CHECK(ring.pendingBytes() == 2);

    unsigned char out[8] = {0};
    CHECK(ring.read(out, 8) == 2);
    CHECK(out[0] == 8 && out[1] == 9);
}

static void TestRingDiscardKeepsSpaceUntilApplied() {
    SpscRing ring;
    ring.allocate(4);
    const unsigned char data[4] = {1, 2, 3, 4};
    ring.write(data, 4);
    ring.requestDiscard();

    // The consumer might still be copying these bytes, so they stay reserved for now.
    CHECK(ring.freeBytes() == 0);
    ring.applyDiscard();
    CHECK(ring.freeBytes() == 4);
}

static void TestRingAcrossThreads() {
    SpscRing ring;
    ring.allocate(1021);  // A prime size exercises every wrap-around position
    const uint32_t kValues = 500000;
    std::atomic<bool> corrupt{false};

    std::thread producer([&] {
        uint32_t next = 0;
        while (next < kValues) {
            if (ring.freeBytes() >= sizeof(next)) {
                ring.write((const unsigned char*)&next, sizeof(next));
                ++next;
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Values are written whole, so they must arrive complete and in their original order.
    uint32_t expected = 0;
    unsigned char bytes[sizeof(uint32_t)];
    size_t have = 0;
    while (expected < kValues) {
        have += ring.read(bytes + have, sizeof(bytes) - have);
        if (have == sizeof(bytes)) {
            uint32_t value;
            memcpy(&value, bytes, sizeof(value));
            if (value != expected) corrupt.store(true);
            ++expected;
            have = 0;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    CHECK(!corrupt.load());
}

// --- Sample conversion ------------------------------------------------------

static void TestConvert16BitIsBitPerfect() {
    const int16_t in[4] = {0, 1, -1, 32767};
    int16_t out[4] = {0};
    ConvertFrames((const unsigned char*)in, (unsigned char*)out, 2, 16, 2, OutFormat{16, 16, false}, NULL);
    CHECK(memcmp(in, out, sizeof(in)) == 0);
}

static void TestConvert16To24InContainer32() {
    const int16_t in[1] = {0x1234};
    uint32_t out[1] = {0xFFFFFFFFu};
    ConvertFrames((const unsigned char*)in, (unsigned char*)out, 1, 16, 1, OutFormat{32, 24, false}, NULL);
    CHECK(out[0] == 0x12340000u);
}

static void TestConvert16ToFloat() {
    const int16_t in[3] = {-32768, 0, 16384};
    float out[3] = {0};
    ConvertFrames((const unsigned char*)in, (unsigned char*)out, 3, 16, 1, OutFormat{32, 32, true}, NULL);
    CHECK(Near(out[0], -1.0));
    CHECK(Near(out[1], 0.0));
    CHECK(Near(out[2], 0.5));
}

static void TestConvert8BitIsCentredOnSilence() {
    const unsigned char in[2] = {128, 255};  // 128 is silence in unsigned 8-bit audio
    int16_t out[2] = {1, 1};
    ConvertFrames(in, (unsigned char*)out, 2, 8, 1, OutFormat{16, 16, false}, NULL);
    CHECK(out[0] == 0);
    CHECK(out[1] == 0x7F00);
}

static void TestConvertGainLimitsInsteadOfWrapping() {
    const int16_t in[2] = {32767, -32768};
    const float gain[1] = {4.0f};
    int16_t out[2] = {0};
    ConvertFrames((const unsigned char*)in, (unsigned char*)out, 2, 16, 1, OutFormat{16, 16, false}, gain);
    CHECK(out[0] == 32767);
    CHECK(out[1] == -32768);
}

static void TestGains() {
    float gains[2] = {0};
    CHECK(!ComputeGains(255, 0, 2, gains));  // Full volume stays bit-perfect

    CHECK(ComputeGains(0, 0, 2, gains));
    CHECK(gains[0] == 0.f && gains[1] == 0.f);

    CHECK(ComputeGains(255, 64, 2, gains));  // Panned right lowers the left channel
    CHECK(Near(gains[0], 0.5));
    CHECK(Near(gains[1], 1.0));

    CHECK(ComputeGains(255, -64, 2, gains));  // Panned left lowers the right channel
    CHECK(Near(gains[0], 1.0));
    CHECK(Near(gains[1], 0.5));
}

// --- Song position ----------------------------------------------------------

static void TestPositionIgnoresLeadingSilence() {
    PositionTracker tracker;
    tracker.onBufferFilled(0, 100);    // Silence while waiting for the decoder
    tracker.onBufferFilled(500, 0);    // Then real audio
    CHECK(tracker.playedFrames(100) == 0);
    CHECK(tracker.playedFrames(350) == 250);
    CHECK(tracker.submittedFrames() == 500);
}

static void TestPositionStopsAtEndOfAudio() {
    PositionTracker tracker;
    tracker.onBufferFilled(300, 200);  // Trailing silence must not advance the song
    CHECK(tracker.playedFrames(500) == 300);
}

static void TestPositionContinuesAfterDeviceChange() {
    PositionTracker tracker;
    tracker.restartAt(44100);          // A new device's clock starts again at zero
    CHECK(tracker.playedFrames(0) == 44100);
    tracker.onBufferFilled(1000, 0);
    CHECK(tracker.playedFrames(250) == 44350);
    CHECK(tracker.submittedFrames() == 45100);
}

// --- Requests between threads -----------------------------------------------

static void TestPauseKeepsNewestRequest() {
    StreamControl control;
    CHECK(control.setPaused(true) == false);
    CHECK(control.setPaused(false) == true);
    CHECK(control.setPaused(true) == false);
    CHECK(control.wantPaused());
}

static void TestFlushRequestsAreNotLost() {
    StreamControl control;
    const unsigned first = control.requestFlush();
    const unsigned second = control.requestFlush();
    CHECK(control.latestFlush() == second);
    CHECK(!control.flushCompleted(first));

    // Completing the newest seek also satisfies anyone waiting on an older one.
    control.markFlushCompleted(second);
    CHECK(control.flushCompleted(first));
    CHECK(control.flushCompleted(second));
}

// --- Settings ---------------------------------------------------------------

static void TestSettingsAreClamped() {
    Settings low;
    low.ringMs = 1;
    low.periodMs = -5;
    low = ClampSettings(low);
    CHECK(low.ringMs == kMinRingMs);
    CHECK(low.periodMs == kMinPeriodMs);

    Settings high;
    high.ringMs = 999999;
    high.periodMs = 999999;
    high = ClampSettings(high);
    CHECK(high.ringMs == kMaxRingMs);
    CHECK(high.periodMs == kMaxPeriodMs);
}

static void TestSettingsSurviveSaveAndLoad() {
    wchar_t folder[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, folder);
    GetTempFileNameW(folder, L"vwa", 0, path);

    Settings saved;
    saved.deviceId = L"{test-device}";
    saved.exclusive = 0;
    saved.fallback = 0;
    saved.volume = 0;
    saved.ringMs = 750;
    saved.periodMs = 20;
    SaveSettings(path, saved);

    const Settings loaded = LoadSettings(path);
    DeleteFileW(path);
    CHECK(loaded.deviceId == saved.deviceId);
    CHECK(loaded.exclusive == 0 && loaded.fallback == 0 && loaded.volume == 0);
    CHECK(loaded.ringMs == 750);
    CHECK(loaded.periodMs == 20);
}

int main() {
    TestRingStartsEmpty();
    TestRingStopsWhenFull();
    TestRingWrapsAround();
    TestRingDiscardDropsOnlyOldAudio();
    TestRingDiscardKeepsSpaceUntilApplied();
    TestRingAcrossThreads();

    TestConvert16BitIsBitPerfect();
    TestConvert16To24InContainer32();
    TestConvert16ToFloat();
    TestConvert8BitIsCentredOnSilence();
    TestConvertGainLimitsInsteadOfWrapping();
    TestGains();

    TestPositionIgnoresLeadingSilence();
    TestPositionStopsAtEndOfAudio();
    TestPositionContinuesAfterDeviceChange();

    TestPauseKeepsNewestRequest();
    TestFlushRequestsAreNotLost();

    TestSettingsAreClamped();
    TestSettingsSurviveSaveAndLoad();

    if (g_failures) {
        printf("%d check(s) failed.\n", g_failures);
        return 1;
    }
    printf("All tests passed.\n");
    return 0;
}
