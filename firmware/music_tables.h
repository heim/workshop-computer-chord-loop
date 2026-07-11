// music_tables.h
//
// All musical constants for ChordLoop live here so they are easy to tweak by
// hand: scales, chord-type voicings, and the default degree tables that map
// Turing-register group values to chords.
//
// This header is pure data + tiny inline helpers. It has NO Pico SDK / hardware
// dependencies so that it (and everything that includes it) compiles unchanged
// on a host machine for unit testing.
//
// Design note - "never a wrong note":
//   Chords are built by stacking *scale steps* (diatonic thirds) on top of a
//   root scale degree. Because every tone is itself a scale degree, every chord
//   produced from any table entry is guaranteed to be diatonic to the selected
//   scale, for every tonic. This is what lets the register mutate freely without
//   ever producing an out-of-scale note.

#ifndef CHORDLOOP_MUSIC_TABLES_H
#define CHORDLOOP_MUSIC_TABLES_H

#include <cstdint>

namespace chordloop {

// ---------------------------------------------------------------------------
// Sizing constants
// ---------------------------------------------------------------------------
constexpr int kNumScales      = 8;   // Y knob slots
constexpr int kMaxScaleLen    = 7;   // diatonic scales have 7 notes
constexpr int kNumChordTypes  = 8;   // see ChordType below
constexpr int kMaxChordTones  = 5;   // largest voicing (ninth = 5 tones)
constexpr int k2BitTableLen   = 4;   // 2-bit group -> 4 entries
constexpr int k4BitTableLen   = 16;  // 4-bit group -> 16 entries

// ---------------------------------------------------------------------------
// Scales (Y knob, 8 slots)
// ---------------------------------------------------------------------------
enum Scale : uint8_t {
    SCALE_MAJOR = 0,
    SCALE_NAT_MINOR,
    SCALE_HARM_MINOR,
    SCALE_DORIAN,
    SCALE_PHRYGIAN,
    SCALE_MIXOLYDIAN,
    SCALE_LYDIAN,
    SCALE_MIN_PENT,
};

// ---------------------------------------------------------------------------
// Chord types.
//
// Each type is expressed as a set of *scale-step* offsets from the root degree.
// Stacking scale steps keeps every tone inside the scale for any tonic.
//   TRIAD   : 1-3-5   (root, diatonic third, diatonic fifth)
//   SEVENTH : 1-3-5-7
//   SUS2    : 1-2-5
//   SUS4    : 1-4-5
//   SIXTH   : 1-3-5-6
//   NINTH   : 1-3-5-7-9
//   POWER   : 1-5     (no third - always neutral / safe)
//   ADD9    : 1-3-5-9
// ---------------------------------------------------------------------------
enum ChordType : uint8_t {
    TYPE_TRIAD = 0,
    TYPE_SEVENTH,
    TYPE_SUS2,
    TYPE_SUS4,
    TYPE_SIXTH,
    TYPE_NINTH,
    TYPE_POWER,
    TYPE_ADD9,
};

// A single chord choice, as stored in a degree table and written by the web UI.
struct ChordDef {
    uint8_t degree;    // 0..scaleLen-1  : scale degree of the root
    uint8_t type;      // ChordType
    int8_t  inversion; // 0 = root position, 1 = first inversion, ...
    int8_t  octave;    // -1, 0, +1 relative to base
};

// ---------------------------------------------------------------------------
// Scale interval tables (semitone offsets from the tonic).
// scaleLength() gives the number of valid entries.
// ---------------------------------------------------------------------------
extern const int8_t kScaleIntervals[kNumScales][kMaxScaleLen];
extern const uint8_t kScaleLength[kNumScales];

inline int scaleLength(int scale) { return kScaleLength[scale]; }

// ---------------------------------------------------------------------------
// Chord-type voicings: scale-step offsets and tone counts.
// ---------------------------------------------------------------------------
extern const int8_t  kChordSteps[kNumChordTypes][kMaxChordTones];
extern const uint8_t kChordToneCount[kNumChordTypes];

// ---------------------------------------------------------------------------
// Default degree tables (compiled-in fallback + factory reset source).
//   kDefault2Bit[scale][0..3]   - curated, always-musical (lengths 6 & 8)
//   kDefault4Bit[scale][0..15]  - richer diatonic set     (lengths 2,3,4)
// ---------------------------------------------------------------------------
extern const ChordDef kDefault2Bit[kNumScales][k2BitTableLen];
extern const ChordDef kDefault4Bit[kNumScales][k4BitTableLen];

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Returns the semitone offset (relative to the tonic) of a scale index that may
// exceed one octave. e.g. index 9 on a 7-note scale = interval[2] + 12.
inline int scaleStepToSemitone(int scale, int scaleIndex) {
    int len = scaleLength(scale);
    int oct = scaleIndex / len;
    int idx = scaleIndex % len;
    if (idx < 0) { idx += len; oct -= 1; }
    return kScaleIntervals[scale][idx] + 12 * oct;
}

} // namespace chordloop

#endif // CHORDLOOP_MUSIC_TABLES_H
