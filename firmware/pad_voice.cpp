// pad_voice.cpp - internal pad voice implementation.

#include "pad_voice.h"
#include <cmath>

namespace chordloop {

void PadVoice::init() {
    // Phase increment per MIDI note: inc = f / fs * 2^32, f = 440*2^((n-69)/12).
    // Float math here is fine - it runs once at startup, not per sample.
    for (int n = 0; n < 128; ++n) {
        double f = 440.0 * std::pow(2.0, (n - 69) / 12.0);
        double inc = f / (double)kSampleRate * 4294967296.0;
        incTable_[n] = (uint32_t)(inc + 0.5);
    }
    bankA_ = Bank();
    bankB_ = Bank();
    active_ = 0;
    xfade_ = 0;
    lpL_ = lpR_ = 0;
}

void PadVoice::setParams(uint8_t waveform, uint8_t detuneCents,
                         uint8_t cutoff, uint8_t width, bool enabled) {
    waveform_ = waveform ? 1 : 0;
    detune_   = detuneCents > 50 ? 50 : detuneCents;
    cutoff_   = cutoff;
    width_    = width;
    enabled_  = enabled;
    voiceIncrements(bankA_);
    voiceIncrements(bankB_);
}

// Detune spread: voice v is offset by a small +/- cents amount around centre, so
// unison-ish tones beat gently. Applied as a fractional nudge to the increment.
void PadVoice::voiceIncrements(Bank &b) const {
    for (int v = 0; v < b.count; ++v) {
        int note = b.note[v];
        if (note < 0) note = 0;
        if (note > 127) note = 127;
        uint32_t base = incTable_[note];
        // spread -detune..+detune cents across the voices
        int spread = (b.count > 1)
            ? (int)detune_ * (2 * v - (b.count - 1)) / (b.count - 1)
            : 0;
        // factor ~ 1 + cents/1731 ; work in parts-per-2^16 to stay integer
        int32_t ppm = spread * 38; // ~ 2^16 / 1731
        int64_t inc = (int64_t)base + ((int64_t)base * ppm >> 16);
        b.inc[v] = (uint32_t)inc;
    }
}

void PadVoice::setChord(const DecodedChord &chord) {
    // Load the new notes into the inactive bank and start a crossfade.
    Bank &dst = (active_ == 0) ? bankB_ : bankA_;
    int n = chord.count;
    if (n > kMaxPadVoices) n = kMaxPadVoices;
    dst.count = n;
    for (int v = 0; v < n; ++v) {
        dst.note[v]  = chord.notes[v];
        dst.phase[v] = 0; // start in phase; detune pulls them apart over time
    }
    voiceIncrements(dst);
    active_ = 1 - active_;      // inactive bank becomes the new active target
    xfade_  = kPadXfadeSamples; // fade from the old bank to the new one
}

// Band-limited-ish waveform from a 32-bit phase. Returns roughly +/-32768.
int32_t PadVoice::osc(uint32_t phase) const {
    int32_t saw = (int32_t)(phase >> 16) - 32768; // -32768..32767
    if (waveform_ == 0) return saw;
    // Triangle from the saw magnitude.
    int32_t a = saw < 0 ? -saw : saw;             // 0..32768
    return (a - 16384) * 2;                       // ~ -32768..32768
}

void PadVoice::render(int32_t &outL, int32_t &outR) {
    if (!enabled_) { outL = 0; outR = 0; return; }

    // Pan gains (0..256). Even voices lean left, odd lean right; width controls
    // how far from centre.
    const int32_t w = width_;             // 0..255
    const int32_t gWide = 128 + (w >> 1); // 128..255 for the "near" channel
    const int32_t gNarr = 128 - (w >> 1); // 128..1   for the "far" channel

    int newBank = active_;
    int oldBank = 1 - active_;

    int32_t sumL = 0, sumR = 0;

    // gains for crossfade (0..256)
    int32_t gNew = 256, gOld = 0;
    if (xfade_ > 0) {
        gNew = 256 * (kPadXfadeSamples - xfade_) / kPadXfadeSamples;
        gOld = 256 - gNew;
    }

    Bank *banks[2] = { &bankA_, &bankB_ };
    int32_t bankGain[2] = {0, 0};
    bankGain[newBank] = gNew;
    bankGain[oldBank] = (xfade_ > 0) ? gOld : 0;

    for (int bi = 0; bi < 2; ++bi) {
        int32_t bg = bankGain[bi];
        if (bg == 0) continue;
        Bank &b = *banks[bi];
        for (int v = 0; v < b.count; ++v) {
            b.phase[v] += b.inc[v];
            int32_t s = osc(b.phase[v]) >> 7; // +/-256
            int32_t gL = (v & 1) ? gNarr : gWide;
            int32_t gR = (v & 1) ? gWide : gNarr;
            sumL += (s * gL >> 8) * bg >> 8;
            sumR += (s * gR >> 8) * bg >> 8;
        }
    }

    // One-pole low-pass per channel. k in 1..255 from cutoff (higher = brighter).
    int32_t k = 1 + cutoff_;
    lpL_ += ((sumL - lpL_) * k) >> 8;
    lpR_ += ((sumR - lpR_) * k) >> 8;

    int32_t oL = lpL_, oR = lpR_;
    if (oL < -2047) oL = -2047;
    if (oL > 2047)  oL = 2047;
    if (oR < -2047) oR = -2047;
    if (oR > 2047)  oR = 2047;
    outL = oL;
    outR = oR;

    if (xfade_ > 0) --xfade_;
}

} // namespace chordloop
