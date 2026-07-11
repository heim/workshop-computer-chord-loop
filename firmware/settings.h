// settings.h
//
// Card-wide configuration struct and hardware/tuning constants. Pure header,
// no SDK dependencies, shared by the engine, serial protocol, persistence and
// the host tests.

#ifndef CHORDLOOP_SETTINGS_H
#define CHORDLOOP_SETTINGS_H

#include <cstdint>

namespace chordloop {

// ---------------------------------------------------------------------------
// Fixed audio parameters
// ---------------------------------------------------------------------------
constexpr int kSampleRate = 48000;

// ---------------------------------------------------------------------------
// CV input -> pitch conversion (tonic on CV in 1)
//
// ComputerCard CVIn returns a signed ~12-bit count. On the Workshop Computer the
// CV inputs span roughly +/-6 V over that range, giving ~341 counts per volt
// (2048 / 6). This is an UNCALIBRATED input, so treat these as nominal values;
// the quantiser snaps to the nearest semitone which absorbs small scale errors.
// Tweak kCvCountsPerVolt here if bench measurement shows a different slope.
// ---------------------------------------------------------------------------
constexpr int kCvCountsPerVolt   = 341;              // ~2048/6V
constexpr int kCvCountsPerSemi   = kCvCountsPerVolt / 12; // ~28 counts/semitone
constexpr int kCvDeadbandCounts  = 10;               // ~30 mV hysteresis band
constexpr int kTonicBaseMidi     = 48;               // MIDI note at 0 V (C3)
constexpr int kTonicMinMidi      = 24;
constexpr int kTonicMaxMidi      = 96;

// ---------------------------------------------------------------------------
// Clocking
// ---------------------------------------------------------------------------
constexpr int kNumClockDivs = 5;
constexpr uint8_t kClockDivs[kNumClockDivs] = {1, 2, 4, 8, 16};
constexpr int kClockTimeoutSamples = 3 * kSampleRate;   // 3 s -> "waiting"

// Trigger widths (samples @ 48kHz)
constexpr int kChordPulseSamples = kSampleRate / 100;   // 10 ms
constexpr int kArpPulseSamples   = kSampleRate / 200;   // 5 ms
constexpr int kMutationFlashSamples = kSampleRate / 20;  // 50 ms

// ---------------------------------------------------------------------------
// LED layout (indices per ComputerCard, bottom-left of panel):
//     0 1
//     2 3
//     4 5
// Left column 0/2/4 = 3-bit binary of the current step.
// 1 = chord-change pulse, 3 = arp-step pulse, 5 = spare / mutation.
// A mutation briefly flashes all six.
// ---------------------------------------------------------------------------
constexpr int kLedStepBit0 = 0;
constexpr int kLedStepBit1 = 2;
constexpr int kLedStepBit2 = 4;
constexpr int kLedChordPulse = 1;
constexpr int kLedArpPulse   = 3;
constexpr int kLedSpare      = 5;

// ---------------------------------------------------------------------------
// Persisted configuration
// ---------------------------------------------------------------------------
struct Config {
    uint8_t chordClockDiv;       // one of kClockDivs (1/2/4/8/16)
    uint8_t arpPatternOverride;  // 0 = auto (CV in 2 zones), 1..5 = forced
    uint8_t padEnabled;          // 0/1  internal pad voice on the audio outs
    uint8_t padWaveform;         // 0 = saw, 1 = triangle
    uint8_t padDetuneCents;      // detune spread, 0..25 cents
    uint8_t padCutoff;           // one-pole LPF cutoff, 0..255
    uint8_t padWidth;            // stereo width, 0..255
    uint8_t reserved;            // keep struct size even
};

inline Config defaultConfig() {
    Config c;
    c.chordClockDiv      = 8;
    c.arpPatternOverride = 0;   // auto from CV in 2
    c.padEnabled         = 1;
    c.padWaveform        = 0;   // saw
    c.padDetuneCents     = 4;
    c.padCutoff          = 170;
    c.padWidth           = 160;
    c.reserved           = 0;
    return c;
}

// Clamp/validate a clock divisor to the nearest allowed value.
inline uint8_t sanitizeClockDiv(uint8_t d) {
    for (int i = 0; i < kNumClockDivs; ++i)
        if (kClockDivs[i] == d) return d;
    return 8;
}

} // namespace chordloop

#endif // CHORDLOOP_SETTINGS_H
