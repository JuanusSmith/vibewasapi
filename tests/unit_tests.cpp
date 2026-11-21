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

#include <audioclient.h>

#include "fader.h"
#include "playback_status.h"
#include "position_tracker.h"
#include "sample_convert.h"
#include "settings.h"
#include "spsc_ring.h"
#include "stream_control.h"
#include "track_timeline.h"

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

    ring.applyDiscard();
    unsigned char out[8] = {0};
    CHECK(ring.read(out, 8) == 2);
    CHECK(out[0] == 8 && out[1] == 9);
}

static void TestRingReadStopsAtDiscardMarker() {
    SpscRing ring;
    ring.allocate(8);
    const unsigned char oldAudio[3] = {1, 2, 3};
    const unsigned char newAudio[2] = {8, 9};
    ring.write(oldAudio, 3);
    ring.requestDiscard();
    ring.write(newAudio, 2);

    // Before the discard is applied, only the stale audio can be read (for a fade-out).
    unsigned char out[8] = {0};
    CHECK(ring.read(out, 2) == 2);
    CHECK(out[0] == 1 && out[1] == 2);
    CHECK(ring.read(out, 8) == 1);
    CHECK(out[0] == 3);
    CHECK(ring.read(out, 8) == 0);

    // Applying the discard after reading up to the marker loses no new audio.
    ring.applyDiscard();
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

static void TestConvertAppliesFadeRamp() {
    const int16_t in[4] = {16384, 16384, 16384, 16384};
    int16_t out[4] = {0};
    GainRamp ramp = {1.0f, -0.5f, 2, 0.0f};  // 1.0, 0.5, then silence
    ConvertFrames((const unsigned char*)in, (unsigned char*)out, 4, 16, 1, OutFormat{16, 16, false}, NULL, &ramp);
    CHECK(out[0] == 16384);
    CHECK(out[1] == 8192);
    CHECK(out[2] == 0);
    CHECK(out[3] == 0);
}

// --- Dither -----------------------------------------------------------------

static void TestNeedsDitherOnlyWhenDetailIsLost() {
    CHECK(!NeedsDither(16, OutFormat{16, 16, false}, false));  // Bit-perfect
    CHECK(!NeedsDither(24, OutFormat{32, 24, false}, false));  // 24-in-32 holds 24 bits
    CHECK(NeedsDither(24, OutFormat{16, 16, false}, false));   // 24 -> 16 loses 8 bits
    CHECK(NeedsDither(16, OutFormat{16, 16, false}, true));    // Volume adds fractions
    CHECK(!NeedsDither(24, OutFormat{32, 32, true}, true));    // Float keeps the detail
    CHECK(!NeedsDither(32, OutFormat{32, 32, false}, true));   // 32-bit is far below hearing
}

static void TestDitherLeavesBitPerfectAudioAlone() {
    const int16_t in[4] = {0, 1, -1, 32767};
    int16_t out[4] = {0};
    DitherState dither;
    ConvertFrames((const unsigned char*)in, (unsigned char*)out, 2, 16, 2, OutFormat{16, 16, false}, NULL, NULL, &dither);
    CHECK(memcmp(in, out, sizeof(in)) == 0);
}

// Builds 24-bit little-endian samples that all hold `value`.
static std::vector<unsigned char> Make24Bit(int value, size_t count) {
    std::vector<unsigned char> bytes(count * 3);
    for (size_t i = 0; i < count; ++i) {
        bytes[i * 3] = (unsigned char)value;
        bytes[i * 3 + 1] = (unsigned char)(value >> 8);
        bytes[i * 3 + 2] = (unsigned char)(value >> 16);
    }
    return bytes;
}

static void TestDitherStaysWithinOneStep() {
    // 24-bit 1000.25 steps of 16-bit audio: each output must be 999, 1000, 1001 or 1002 at most.
    const size_t count = 5000;
    const std::vector<unsigned char> in = Make24Bit(1000 * 256 + 64, count);
    std::vector<int16_t> out(count);
    DitherState dither;
    ConvertFrames(in.data(), (unsigned char*)out.data(), count, 24, 1, OutFormat{16, 16, false}, NULL, NULL, &dither);
    bool inRange = true, varies = false;
    for (size_t i = 0; i < count; ++i) {
        if (fabs(out[i] - 1000.25) >= 1.5) inRange = false;
        if (out[i] != out[0]) varies = true;
    }
    CHECK(inRange);
    CHECK(varies);  // The noise is really being added
}

static void TestDitherKeepsDetailBelowTheLastBit() {
    // A level of 1000.25 steps can't be stored in 16 bits. Without dither every sample
    // becomes 1000, losing the quarter step; with dither the average keeps it.
    const size_t count = 40000;
    const std::vector<unsigned char> in = Make24Bit(1000 * 256 + 64, count);
    std::vector<int16_t> out(count);

    ConvertFrames(in.data(), (unsigned char*)out.data(), count, 24, 1, OutFormat{16, 16, false}, NULL);
    double plainSum = 0;
    for (size_t i = 0; i < count; ++i) plainSum += out[i];
    CHECK(Near(plainSum / count, 1000.0));

    DitherState dither;
    ConvertFrames(in.data(), (unsigned char*)out.data(), count, 24, 1, OutFormat{16, 16, false}, NULL, NULL, &dither);
    double ditheredSum = 0;
    for (size_t i = 0; i < count; ++i) ditheredSum += out[i];
    CHECK(fabs(ditheredSum / count - 1000.25) < 0.02);
}

static void TestDitherOfSilenceIsOnlyTheSmallestStep() {
    const int16_t in[1000] = {0};
    int16_t out[1000];
    const float gain[1] = {0.5f};
    DitherState dither;
    ConvertFrames((const unsigned char*)in, (unsigned char*)out, 1000, 16, 1, OutFormat{16, 16, false}, gain, NULL, &dither);
    bool tiny = true;
    for (int i = 0; i < 1000; ++i) {
        if (out[i] < -1 || out[i] > 1) tiny = false;
    }
    CHECK(tiny);
}

static void TestDitherStillLimitsLoudAudio() {
    const int16_t in[2] = {32767, -32768};
    const float gain[1] = {4.0f};
    int16_t out[2] = {0};
    DitherState dither;
    ConvertFrames((const unsigned char*)in, (unsigned char*)out, 2, 16, 1, OutFormat{16, 16, false}, gain, NULL, &dither);
    CHECK(out[0] == 32767);
    CHECK(out[1] == -32768);
}

// --- Status text ------------------------------------------------------------

static bool Contains(const std::wstring& text, const wchar_t* part) {
    return text.find(part) != std::wstring::npos;
}

static void TestDescribeRatesAndFormats() {
    CHECK(DescribeRate(44100) == L"44.1 kHz");
    CHECK(DescribeRate(48000) == L"48 kHz");
    CHECK(DescribeRate(176400) == L"176.4 kHz");
    CHECK(DescribeSampleFormat(OutFormat{16, 16, false}) == L"16-bit");
    CHECK(DescribeSampleFormat(OutFormat{32, 24, false}) == L"24-bit in 32");
    CHECK(DescribeSampleFormat(OutFormat{32, 32, true}) == L"32-bit float");
}

static void TestStatusShowsBitPerfect() {
    PlaybackStatus status;
    status.deviceName = L"Headphones (U18)";
    status.exclusive = true;
    status.sampleRate = 48000;
    status.sourceBits = 16;
    status.out = OutFormat{16, 16, false};
    status.latencyMs = 30;
    const std::wstring text = DescribePlayback(status);
    CHECK(Contains(text, L"Headphones (U18): exclusive mode, 48 kHz, 16-bit, stereo, 30 ms latency."));
    CHECK(Contains(text, L"Bit-perfect"));

    status.softwareVolume = true;  // Turning the volume down ends bit-perfect playback
    CHECK(Contains(DescribePlayback(status), L"volume is applied in software, with dither"));

    CHECK(!Contains(text, L"Paused"));
    status.paused = true;
    CHECK(Contains(DescribePlayback(status), L"30 ms latency. Paused."));
}

static void TestStatusShowsReducedBitDepth() {
    PlaybackStatus status;
    status.exclusive = true;
    status.sampleRate = 48000;
    status.sourceBits = 24;
    status.out = OutFormat{16, 16, false};
    const std::wstring text = DescribePlayback(status);
    CHECK(Contains(text, L"The 24-bit source is reduced to 16-bit, with dither."));
    CHECK(!Contains(text, L"Bit-perfect"));
}

static void TestStatusExplainsFallback() {
    PlaybackStatus status;
    status.exclusive = false;
    status.exclusiveError = AUDCLNT_E_UNSUPPORTED_FORMAT;
    status.sampleRate = 44100;
    const std::wstring text = DescribePlayback(status);
    CHECK(Contains(text, L"shared mode"));
    CHECK(Contains(text, L"Exclusive mode wasn't used: the device doesn't accept 44.1 kHz stereo audio in exclusive mode."));

    CHECK(Contains(DescribeOpenError(AUDCLNT_E_DEVICE_IN_USE, 44100, 2), L"another app"));
    CHECK(Contains(DescribeOpenError(E_FAIL, 44100, 2), L"0x80004005"));
}

static void TestDescribeExclusiveFormats() {
    std::vector<RateSupport> rates(2);
    rates[0].sampleRate = 44100;
    rates[1].sampleRate = 48000;
    rates[1].formats.push_back(OutFormat{16, 16, false});
    rates[1].formats.push_back(OutFormat{32, 32, true});
    const std::wstring text = DescribeExclusiveFormats(S_OK, rates);
    CHECK(Contains(text, L"44.1 kHz: not accepted\r\n"));
    CHECK(text == L"44.1 kHz: not accepted\r\n48 kHz: 16-bit, 32-bit float");

    rates[1].formats.clear();
    CHECK(Contains(DescribeExclusiveFormats(S_OK, rates), L"use shared mode"));
    CHECK(Contains(DescribeExclusiveFormats(AUDCLNT_E_DEVICE_IN_USE, rates), L"Stop playback"));
}

// --- Track timeline ---------------------------------------------------------

static void TestTimelineCountsAFreshTrack() {
    TrackTimeline timeline;
    timeline.startStream(48000);
    timeline.onWritten(96000);                     // Two seconds written
    CHECK(timeline.writtenTimeMs() == 2000);
    CHECK(timeline.outputTimeMs(24000) == 500);    // Half a second heard
}

static void TestTimelineGaplessTrackStartsAtZero() {
    TrackTimeline timeline;
    timeline.startStream(48000);
    timeline.onWritten(480000);                    // Track 1: ten seconds
    timeline.startGaplessTrack();
    timeline.onWritten(48000);                     // Track 2: one second so far

    // The listener is still hearing the end of track 1, so track 2 shows 0:00.
    CHECK(timeline.outputTimeMs(470000) == 0);
    // Once playback passes the join, track 2's own time counts up.
    CHECK(timeline.outputTimeMs(480000 + 24000) == 500);
    CHECK(timeline.writtenTimeMs() == 1000);
}

static void TestTimelineSeekAddsTheTarget() {
    TrackTimeline timeline;
    timeline.startStream(44100);
    timeline.onWritten(441000);
    timeline.seek(90000);                          // Seek to 1:30
    timeline.finishSeek();                         // The engine has carried it out
    CHECK(timeline.outputTimeMs(0) == 90000);
    timeline.onWritten(44100);
    CHECK(timeline.writtenTimeMs() == 91000);
    CHECK(timeline.outputTimeMs(22050) == 90500);
}

// While a seek is being carried out, the engine's played count still belongs to the
// audio before the seek. The time shown must be the target, not target + old count.
static void TestTimelineShowsTargetDuringSeek() {
    TrackTimeline timeline;
    timeline.startStream(44100);
    timeline.onWritten(44100 * 20);
    const long long playedBeforeSeek = 44100 * 14;   // 14 seconds heard so far

    timeline.seek(90000);                            // Seek to 1:30 begins
    CHECK(timeline.outputTimeMs(playedBeforeSeek) == 90000);  // Not 1:44

    timeline.finishSeek();                           // Engine's count restarts at zero
    CHECK(timeline.outputTimeMs(0) == 90000);
    CHECK(timeline.outputTimeMs(44100) == 91000);    // And counts up from the target
}

static void TestTimelineEndReportClearsOnNewAudio() {
    TrackTimeline timeline;
    timeline.startStream(44100);
    CHECK(!timeline.endReported());
    timeline.markEndReported();
    CHECK(timeline.endReported());
    timeline.onWritten(0);                         // Nothing written: still the end
    CHECK(timeline.endReported());
    timeline.onWritten(100);                       // More audio: not the end after all
    CHECK(!timeline.endReported());
    timeline.markEndReported();
    timeline.startGaplessTrack();                  // A new track is never "ended"
    CHECK(!timeline.endReported());
}

// --- Fades ------------------------------------------------------------------

static void TestFaderStartsUntouched() {
    Fader fader;
    CHECK(fader.isUnity());
    CHECK(!fader.isFading());
    CHECK(fader.audibleFramesLeft() == -1);
}

static void TestFaderFadesOutAndLimitsAudio() {
    Fader fader;
    fader.fadeTo(0.f, 100);
    CHECK(fader.audibleFramesLeft() == 100);

    GainRamp first = fader.take(40);
    CHECK(Near(first.start, 1.0));
    CHECK(first.frames == 40);
    CHECK(Near(first.end, 0.6));
    CHECK(fader.audibleFramesLeft() == 60);

    GainRamp rest = fader.take(500);  // More frames than remain in the fade
    CHECK(rest.frames == 60);
    CHECK(rest.end == 0.f);
    CHECK(fader.isSilent());
    CHECK(fader.audibleFramesLeft() == 0);
}

static void TestFaderFadesInToUnity() {
    Fader fader;
    fader.setLevel(0.f);
    fader.fadeTo(1.f, 10);
    CHECK(fader.audibleFramesLeft() == -1);  // Heading up, so no limit on audio
    fader.take(10);
    CHECK(fader.isUnity());
}

static void TestFaderReversesMidFade() {
    Fader fader;
    fader.fadeTo(0.f, 100);
    fader.take(50);                // Halfway down
    fader.fadeTo(1.f, 50);         // Pause cancelled: back up from 0.5
    GainRamp ramp = fader.take(50);
    CHECK(Near(ramp.start, 0.5));
    CHECK(Near(ramp.step, 0.01));
    CHECK(fader.isUnity());
}

// A fade-out whose audio runs out must count as finished, or pause and seek would
// wait for their time limit.
static void TestFaderFinishesFadeOutWhenAudioRunsOut() {
    Fader fader;
    fader.fadeTo(0.f, 100);
    fader.take(30);                // Only 30 frames of old audio were left
    CHECK(!fader.isSilent());
    fader.finishFadeOut();
    CHECK(fader.isSilent());
    CHECK(fader.audibleFramesLeft() == 0);
}

// finishFadeOut must leave normal playback and fade-ins alone.
static void TestFinishFadeOutIgnoresOtherStates() {
    Fader steady;
    steady.finishFadeOut();
    CHECK(steady.isUnity());       // Full level stays full (and bit-perfect)

    Fader fadingIn;
    fadingIn.setLevel(0.f);
    fadingIn.fadeTo(1.f, 100);
    fadingIn.take(50);
    fadingIn.finishFadeOut();
    CHECK(fadingIn.isFading());    // Still rising towards full level
    fadingIn.take(50);
    CHECK(fadingIn.isUnity());
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
    saved.exclusive = false;
    saved.fallback = false;
    saved.volume = false;
    saved.ringMs = 750;
    saved.periodMs = 20;
    SaveSettings(path, saved);

    const Settings loaded = LoadSettings(path);
    DeleteFileW(path);
    CHECK(loaded.deviceId == saved.deviceId);
    CHECK(!loaded.exclusive && !loaded.fallback && !loaded.volume);
    CHECK(loaded.ringMs == 750);
    CHECK(loaded.periodMs == 20);
}

int main() {
    TestRingStartsEmpty();
    TestRingStopsWhenFull();
    TestRingWrapsAround();
    TestRingDiscardDropsOnlyOldAudio();
    TestRingReadStopsAtDiscardMarker();
    TestRingDiscardKeepsSpaceUntilApplied();
    TestRingAcrossThreads();

    TestConvert16BitIsBitPerfect();
    TestConvert16To24InContainer32();
    TestConvert16ToFloat();
    TestConvert8BitIsCentredOnSilence();
    TestConvertGainLimitsInsteadOfWrapping();
    TestGains();
    TestConvertAppliesFadeRamp();

    TestNeedsDitherOnlyWhenDetailIsLost();
    TestDitherLeavesBitPerfectAudioAlone();
    TestDitherStaysWithinOneStep();
    TestDitherKeepsDetailBelowTheLastBit();
    TestDitherOfSilenceIsOnlyTheSmallestStep();
    TestDitherStillLimitsLoudAudio();

    TestDescribeRatesAndFormats();
    TestStatusShowsBitPerfect();
    TestStatusShowsReducedBitDepth();
    TestStatusExplainsFallback();
    TestDescribeExclusiveFormats();

    TestFaderStartsUntouched();
    TestFaderFadesOutAndLimitsAudio();
    TestFaderFadesInToUnity();
    TestFaderReversesMidFade();
    TestFaderFinishesFadeOutWhenAudioRunsOut();
    TestFinishFadeOutIgnoresOtherStates();

    TestPositionIgnoresLeadingSilence();
    TestPositionStopsAtEndOfAudio();
    TestPositionContinuesAfterDeviceChange();

    TestTimelineCountsAFreshTrack();
    TestTimelineGaplessTrackStartsAtZero();
    TestTimelineSeekAddsTheTarget();
    TestTimelineShowsTargetDuringSeek();
    TestTimelineEndReportClearsOnNewAudio();

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
