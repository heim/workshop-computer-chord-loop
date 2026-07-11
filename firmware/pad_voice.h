// pad_voice.h
//
// Internal pad voice for the audio outputs: one phase-accumulator oscillator per
// chord tone (up to 4), slight detune spread, voices alternately panned L/R, a
// one-pole low-pass per channel, and a short crossfade on chord changes to avoid
// clicks. Fixed-point throughout (a float lookup table is built once at init).
//
// No SDK includes -> also compiles on the host.

#ifndef CHORDLOOP_PAD_VOICE_H
#define CHORDLOOP_PAD_VOICE_H

#include <cstdint>
#include "chord_engine.h" // DecodedChord
#include "settings.h"     // kSampleRate

namespace chordloop {

constexpr int kMaxPadVoices  = 4;
constexpr int kPadXfadeSamples = kSampleRate / 50; // 20 ms crossfade

class PadVoice {
public:
    void init();
    void setParams(uint8_t waveform, uint8_t detuneCents,
                   uint8_t cutoff, uint8_t width, bool enabled);
    void setChord(const DecodedChord &chord); // start a crossfade to new notes
    void render(int32_t &outL, int32_t &outR); // per-sample, ~ +/-2047

private:
    struct Bank {
        uint32_t phase[kMaxPadVoices] = {0};
        uint32_t inc[kMaxPadVoices]   = {0};
        int8_t   note[kMaxPadVoices]  = {0};
        int      count = 0;
    };

    int32_t osc(uint32_t phase) const;      // waveform sample, +/-32768
    void    voiceIncrements(Bank &b) const; // fill inc[] from note[]+detune

    uint32_t incTable_[128] = {0};   // phase increment per MIDI note

    Bank bankA_, bankB_;
    int  active_   = 0;              // 0 = A, 1 = B
    int  xfade_    = 0;              // remaining crossfade samples (0 = settled)

    // filter state (per channel)
    int32_t lpL_ = 0, lpR_ = 0;

    uint8_t waveform_ = 0;
    uint8_t detune_   = 4;
    uint8_t cutoff_   = 170;
    uint8_t width_    = 160;
    bool    enabled_  = true;
};

} // namespace chordloop

#endif // CHORDLOOP_PAD_VOICE_H
