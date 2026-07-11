// music_tables.cpp - musical constant data for ChordLoop.
//
// Everything here is hand-tweakable. Default degree tables are chosen to be
// musically defensible starting points; they are fully user-editable from the
// web UI and are also the source used for "restore defaults".

#include "music_tables.h"

namespace chordloop {

// ---------------------------------------------------------------------------
// Scale interval tables (semitones from tonic).
// 7-note scales fill all slots; pentatonic uses the first 5 (rest padded).
// ---------------------------------------------------------------------------
const int8_t kScaleIntervals[kNumScales][kMaxScaleLen] = {
    /* MAJOR       */ {0, 2, 4, 5, 7, 9, 11},
    /* NAT_MINOR   */ {0, 2, 3, 5, 7, 8, 10},
    /* HARM_MINOR  */ {0, 2, 3, 5, 7, 8, 11},
    /* DORIAN      */ {0, 2, 3, 5, 7, 9, 10},
    /* PHRYGIAN    */ {0, 1, 3, 5, 7, 8, 10},
    /* MIXOLYDIAN  */ {0, 2, 4, 5, 7, 9, 10},
    /* LYDIAN      */ {0, 2, 4, 6, 7, 9, 11},
    /* MIN_PENT    */ {0, 3, 5, 7, 10, 0, 0},
};

const uint8_t kScaleLength[kNumScales] = {7, 7, 7, 7, 7, 7, 7, 5};

// ---------------------------------------------------------------------------
// Chord-type voicings as scale-step offsets from the root degree.
// Unused slots are -1; kChordToneCount gives the number of active tones.
// ---------------------------------------------------------------------------
const int8_t kChordSteps[kNumChordTypes][kMaxChordTones] = {
    /* TRIAD   */ {0, 2, 4, -1, -1},
    /* SEVENTH */ {0, 2, 4, 6, -1},
    /* SUS2    */ {0, 1, 4, -1, -1},
    /* SUS4    */ {0, 3, 4, -1, -1},
    /* SIXTH   */ {0, 2, 4, 5, -1},
    /* NINTH   */ {0, 2, 4, 6, 8},
    /* POWER   */ {0, 4, -1, -1, -1},
    /* ADD9    */ {0, 2, 4, 8, -1},
};

const uint8_t kChordToneCount[kNumChordTypes] = {3, 4, 3, 3, 4, 5, 2, 4};

// ---------------------------------------------------------------------------
// Default 2-bit tables (4 curated, always-musical chords per scale).
// Used for progression lengths 6 and 8.
// ---------------------------------------------------------------------------
const ChordDef kDefault2Bit[kNumScales][k2BitTableLen] = {
    /* MAJOR      : I  IV  V  vi   */ {{0,TYPE_TRIAD,0,0},{3,TYPE_TRIAD,0,0},{4,TYPE_TRIAD,0,0},{5,TYPE_TRIAD,0,0}},
    /* NAT_MINOR  : i  VI  III VII */ {{0,TYPE_TRIAD,0,0},{5,TYPE_TRIAD,0,0},{2,TYPE_TRIAD,0,0},{6,TYPE_TRIAD,0,0}},
    /* HARM_MINOR : i  iv  V  VI   */ {{0,TYPE_TRIAD,0,0},{3,TYPE_TRIAD,0,0},{4,TYPE_TRIAD,0,0},{5,TYPE_TRIAD,0,0}},
    /* DORIAN     : i  IV  v  VII  */ {{0,TYPE_TRIAD,0,0},{3,TYPE_TRIAD,0,0},{4,TYPE_TRIAD,0,0},{6,TYPE_TRIAD,0,0}},
    /* PHRYGIAN   : i  II  VII iv  */ {{0,TYPE_TRIAD,0,0},{1,TYPE_TRIAD,0,0},{6,TYPE_TRIAD,0,0},{3,TYPE_TRIAD,0,0}},
    /* MIXOLYDIAN : I  VII IV v    */ {{0,TYPE_TRIAD,0,0},{6,TYPE_TRIAD,0,0},{3,TYPE_TRIAD,0,0},{4,TYPE_TRIAD,0,0}},
    /* LYDIAN     : I  II  V  vi   */ {{0,TYPE_TRIAD,0,0},{1,TYPE_TRIAD,0,0},{4,TYPE_TRIAD,0,0},{5,TYPE_TRIAD,0,0}},
    /* MIN_PENT   : i  iv  v  III  */ {{0,TYPE_TRIAD,0,0},{3,TYPE_TRIAD,0,0},{4,TYPE_TRIAD,0,0},{2,TYPE_TRIAD,0,0}},
};

// A generic 16-entry layout that adapts to any 7-note diatonic scale: because
// entries are addressed by scale degree, each scale's own chord qualities fall
// out automatically (e.g. degree 4 is a major V in major, a major V in harmonic
// minor, a minor v in dorian, etc.).
#define DIATONIC_4BIT                        \
    {{0,TYPE_TRIAD,0,0},   /* 0  I      */    \
     {1,TYPE_TRIAD,0,0},   /* 1  ii     */    \
     {2,TYPE_TRIAD,0,0},   /* 2  iii    */    \
     {3,TYPE_TRIAD,0,0},   /* 3  IV     */    \
     {4,TYPE_TRIAD,0,0},   /* 4  V      */    \
     {5,TYPE_TRIAD,0,0},   /* 5  vi     */    \
     {6,TYPE_TRIAD,0,0},   /* 6  vii    */    \
     {4,TYPE_SEVENTH,0,0}, /* 7  V7     */    \
     {0,TYPE_SEVENTH,0,0}, /* 8  Imaj7  */    \
     {0,TYPE_SUS4,0,0},    /* 9  Isus4  */    \
     {4,TYPE_SUS4,0,0},    /* 10 Vsus4  */    \
     {3,TYPE_SEVENTH,0,0}, /* 11 IV7    */    \
     {5,TYPE_SEVENTH,0,0}, /* 12 vi7    */    \
     {0,TYPE_TRIAD,1,0},   /* 13 I/3    */    \
     {4,TYPE_TRIAD,1,0},   /* 14 V/3    */    \
     {0,TYPE_TRIAD,0,1}}   /* 15 I (8va)*/

// ---------------------------------------------------------------------------
// Default 4-bit tables (16 entries per scale) for lengths 2, 3, 4.
// ---------------------------------------------------------------------------
const ChordDef kDefault4Bit[kNumScales][k4BitTableLen] = {
    /* MAJOR      */ DIATONIC_4BIT,
    /* NAT_MINOR  */ DIATONIC_4BIT,
    /* HARM_MINOR */ DIATONIC_4BIT,
    /* DORIAN     */ DIATONIC_4BIT,
    /* PHRYGIAN   */ DIATONIC_4BIT,
    /* MIXOLYDIAN */ DIATONIC_4BIT,
    /* LYDIAN     */ DIATONIC_4BIT,
    /* MIN_PENT (5 degrees, no diatonic 7th chords - use powers/adds) */
    {{0,TYPE_TRIAD,0,0}, {1,TYPE_TRIAD,0,0}, {2,TYPE_TRIAD,0,0}, {3,TYPE_TRIAD,0,0},
     {4,TYPE_TRIAD,0,0}, {0,TYPE_POWER,0,0}, {3,TYPE_POWER,0,0}, {0,TYPE_ADD9,0,0},
     {2,TYPE_TRIAD,0,0}, {4,TYPE_TRIAD,0,0}, {0,TYPE_SEVENTH,0,0},{3,TYPE_TRIAD,0,0},
     {1,TYPE_TRIAD,0,0}, {0,TYPE_TRIAD,1,0}, {3,TYPE_TRIAD,1,0}, {0,TYPE_TRIAD,0,1}},
};

#undef DIATONIC_4BIT

} // namespace chordloop
