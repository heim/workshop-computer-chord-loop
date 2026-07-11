// chord_engine.cpp - Turing register + chord decode implementation.

#include "chord_engine.h"
#include "settings.h"

namespace chordloop {

const uint8_t kLengths[kNumLengths] = {2, 3, 4, 6, 8};

// Integer round-to-nearest division (denominator assumed > 0).
static inline int divRoundNearest(int n, int d) {
    if (d <= 0) return 0;
    return (n >= 0) ? (n + d / 2) / d : -(((-n) + d / 2) / d);
}

// ---------------------------------------------------------------------------
// Free-function decode (shared by firmware, tests, and mirrored in the web UI)
// ---------------------------------------------------------------------------
DecodedChord decodeChord(int scale, const ChordDef &def, int tonicMidi) {
    DecodedChord dc;
    if (scale < 0 || scale >= kNumScales) scale = 0;
    int type = def.type;
    if (type < 0 || type >= kNumChordTypes) type = TYPE_TRIAD;

    int len   = scaleLength(scale);
    int root  = def.degree % len;
    int count = kChordToneCount[type];
    int base  = tonicMidi + def.octave * 12;

    // Work in plain int - notes can transiently exceed the int8_t range while
    // stacking/inverting, so we only narrow (and clamp) at the very end.
    int tmp[kMaxChordTones] = {0};
    for (int j = 0; j < count; ++j) {
        int step = kChordSteps[type][j];
        tmp[j] = base + scaleStepToSemitone(scale, root + step);
    }
    dc.count = static_cast<uint8_t>(count);
    int rootNote = (count > 0) ? tmp[0] : base; // degree root, before inversion

    // Inversion: raise the lowest `inv` tones by an octave.
    int inv = def.inversion;
    if (inv < 0) inv = 0;
    if (count > 0) inv %= count;
    for (int k = 0; k < inv; ++k) tmp[k] += 12;

    // Insertion sort ascending (count <= 5).
    for (int a = 1; a < count; ++a) {
        int key = tmp[a];
        int b = a - 1;
        while (b >= 0 && tmp[b] > key) { tmp[b + 1] = tmp[b]; --b; }
        tmp[b + 1] = key;
    }

    // Narrow into the valid MIDI range used by CVOutMIDINote.
    for (int j = 0; j < kMaxChordTones; ++j) {
        int v = (j < count) ? tmp[j] : 0;
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        dc.notes[j] = static_cast<int8_t>(v);
    }
    if (rootNote < 0) rootNote = 0;
    if (rootNote > 127) rootNote = 127;
    dc.root = static_cast<int8_t>(rootNote);
    dc.bass = (count > 0) ? dc.notes[0] : dc.root;
    return dc;
}

// ---------------------------------------------------------------------------
// ChordEngine
// ---------------------------------------------------------------------------
ChordEngine::ChordEngine() { loadDefaultTables(); }

void ChordEngine::init(uint32_t seed) {
    rng_      = seed ? seed : 0x1234abcdu;
    reg_      = 0xACE1;
    stepIdx_  = 0;
    lockMask_ = 0;
    mutated_  = false;
    tonicInit_ = false;
}

void ChordEngine::loadDefaultTables() {
    for (int s = 0; s < kNumScales; ++s) {
        for (int i = 0; i < k2BitTableLen; ++i) table2_[s][i] = kDefault2Bit[s][i];
        for (int i = 0; i < k4BitTableLen; ++i) table4_[s][i] = kDefault4Bit[s][i];
    }
}

uint32_t ChordEngine::nextRand() {
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
}

void ChordEngine::setScale(int s) {
    if (s < 0) s = 0;
    if (s >= kNumScales) s = kNumScales - 1;
    scaleIdx_ = static_cast<uint8_t>(s);
}

void ChordEngine::setLengthIndex(int li) {
    if (li < 0) li = 0;
    if (li >= kNumLengths) li = kNumLengths - 1;
    lengthIdx_ = static_cast<uint8_t>(li);
}

// Triangular probability curve (Turing-Machine feel):
//   knob fully CW  -> p = 0 (locked loop)
//   knob at noon   -> p = 1 (constant variation)
//   knob fully CCW -> p = 0 (locked; "double length" flag set, see README)
// A small dead-band at each extreme guarantees a truly locked loop there (p==0
// exactly), rather than the ~1-in-2000 residual a bare triangle would leave.
void ChordEngine::setProbabilityKnob(int32_t k) {
    if (k < 0) k = 0;
    if (k > 4095) k = 4095;
    int dist = k - 2048;
    if (dist < 0) dist = -dist;              // 0..2048
    const int active = 2048 - 64;            // 64-count locked dead-band per end
    pQ_ = ((active - dist) * 4096) / active; // 4096 at noon, 0 by the dead-band
    if (pQ_ < 0) pQ_ = 0;
    if (pQ_ > 4096) pQ_ = 4096;
    doubleLen_ = (k < 2048);
}

void ChordEngine::setHardLock(bool on) { hardLock_ = on; }

int ChordEngine::groupValueAt(int step) const {
    int gs = groupSize();
    int shift = step * gs;
    return (reg_ >> shift) & ((1 << gs) - 1);
}

void ChordEngine::onChordClock() {
    int len = length();
    stepIdx_ = static_cast<uint8_t>((stepIdx_ + 1) % len);
    mutated_ = false;

    if (hardLock_) return;
    if (lockMask_ & (1u << stepIdx_)) return;

    int gs = groupSize();
    uint32_t r = nextRand() % 4096u;
    if (static_cast<int>(r) < pQ_) {
        int nflip = 1 + ((pQ_ * (gs - 1)) >> 12); // 1..gs, scales with chaos
        if (nflip < 1) nflip = 1;
        if (nflip > gs) nflip = gs;

        uint32_t mask = 0;
        int placed = 0, guard = 0;
        while (placed < nflip && guard < 32) {
            int b = nextRand() % gs;
            uint32_t bit = 1u << b;
            if (!(mask & bit)) { mask |= bit; ++placed; }
            ++guard;
        }
        reg_ ^= static_cast<uint16_t>(mask << (stepIdx_ * gs));
        mutated_ = (mask != 0);
    }
}

void ChordEngine::resetStep() { stepIdx_ = 0; }

void ChordEngine::randomizeRegister() {
    reg_ = static_cast<uint16_t>(nextRand() & 0xFFFFu);
    mutated_ = true;
}

int ChordEngine::quantizeTonic(int cvCounts) {
    int candidate = divRoundNearest(cvCounts * 12, kCvCountsPerVolt);
    if (!tonicInit_) {
        tonicSemi_ = candidate;
        tonicInit_ = true;
    } else {
        int center = (tonicSemi_ * kCvCountsPerVolt) / 12;    // counts at note center
        int half   = kCvCountsPerVolt / 24;                   // half semitone
        if (cvCounts > center + half + kCvDeadbandCounts ||
            cvCounts < center - half - kCvDeadbandCounts) {
            tonicSemi_ = candidate;
        }
    }
    int t = kTonicBaseMidi + tonicSemi_;
    if (t < kTonicMinMidi) t = kTonicMinMidi;
    if (t > kTonicMaxMidi) t = kTonicMaxMidi;
    tonic_ = t;
    return tonic_;
}

ChordDef ChordEngine::currentChordDef() const {
    int v = currentGroupValue();
    if (groupSize() == 2) {
        if (v < 0 || v >= k2BitTableLen) v = 0;
        return table2_[scaleIdx_][v];
    } else {
        if (v < 0 || v >= k4BitTableLen) v = 0;
        return table4_[scaleIdx_][v];
    }
}

DecodedChord ChordEngine::currentChord() const {
    return decodeChord(scaleIdx_, currentChordDef(), tonic_);
}

void ChordEngine::setStepValue(int step, int value) {
    if (step < 0 || step >= length()) return;
    int gs = groupSize();
    int shift = step * gs;
    uint16_t m = static_cast<uint16_t>(((1 << gs) - 1) << shift);
    reg_ = static_cast<uint16_t>((reg_ & ~m) | ((value << shift) & m));
}

void ChordEngine::setLock(int step, bool on) {
    if (step < 0 || step >= 16) return;
    if (on) lockMask_ |= (1u << step);
    else    lockMask_ &= ~(1u << step);
}

bool ChordEngine::isLocked(int step) const {
    if (step < 0 || step >= 16) return false;
    return (lockMask_ >> step) & 1u;
}

ChordDef *ChordEngine::tableSlot(int scale, int groupBits, int slot) {
    if (scale < 0 || scale >= kNumScales) return nullptr;
    if (groupBits == 2) {
        if (slot < 0 || slot >= k2BitTableLen) return nullptr;
        return &table2_[scale][slot];
    } else if (groupBits == 4) {
        if (slot < 0 || slot >= k4BitTableLen) return nullptr;
        return &table4_[scale][slot];
    }
    return nullptr;
}

} // namespace chordloop
