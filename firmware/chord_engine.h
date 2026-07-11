// chord_engine.h
//
// The musical heart of ChordLoop: a Turing-Machine-style 16-bit shift register
// whose group values are decoded into diatonic chords.
//
// Pure logic - NO Pico SDK includes - so it compiles and is unit-tested on the
// host. main.cpp drives it from ProcessSample().

#ifndef CHORDLOOP_CHORD_ENGINE_H
#define CHORDLOOP_CHORD_ENGINE_H

#include <cstdint>
#include "music_tables.h"

namespace chordloop {

// Progression lengths selectable with the X knob.
// Lengths 2..4 use 4-bit register groups (rich 16-entry table); lengths 6 & 8
// use 2-bit groups (curated 4-entry table). All fit inside 16 bits:
//   4 groups x 4 bits = 16,   8 groups x 2 bits = 16.
constexpr int kNumLengths = 5;
extern const uint8_t kLengths[kNumLengths]; // {2,3,4,6,8}

// A decoded, voiced chord: MIDI note numbers ready for CV output.
struct DecodedChord {
    int8_t  notes[kMaxChordTones]; // voiced tones, ascending
    uint8_t count;                 // number of active tones
    int8_t  bass;                  // lowest voiced tone -> CV out 1
    int8_t  root;                  // degree root (pre-inversion) for reference
};

// Decode a single ChordDef against a tonic. Free function so the unit tests and
// (conceptually) the web UI share exactly one decode definition.
DecodedChord decodeChord(int scale, const ChordDef &def, int tonicMidi);

class ChordEngine {
public:
    ChordEngine();

    // ---- setup -----------------------------------------------------------
    void   init(uint32_t seed);
    void   loadDefaultTables();          // (re)load compiled-in defaults

    // ---- live control (from knobs/switch) --------------------------------
    void   setScale(int s);              // 0..kNumScales-1
    void   setLengthIndex(int li);       // 0..kNumLengths-1
    void   setProbabilityKnob(int32_t knobVal); // Main knob 0..4095 -> p curve
    void   setHardLock(bool on);         // Z-up overrides knob

    // ---- clock / transport ----------------------------------------------
    void   onChordClock();               // advance playhead + maybe mutate
    void   resetStep();                  // Pulse-in-2: playhead -> 0 (keep reg)
    void   randomizeRegister();          // Z-down momentary: scramble all bits

    // ---- tonic (CV in 1) -------------------------------------------------
    // Quantise a raw CV-input count to a MIDI tonic, with hysteresis so a noisy
    // input does not chatter between adjacent semitones. Call on chord-clock
    // boundaries. Returns the current tonic MIDI note.
    int    quantizeTonic(int cvCounts);
    int    tonic() const { return tonic_; }

    // ---- decode ----------------------------------------------------------
    int    groupValueAt(int step) const; // raw group value at a step index
    int    currentGroupValue() const { return groupValueAt(stepIdx_); }
    ChordDef currentChordDef() const;
    DecodedChord currentChord() const;

    // ---- web-UI edits ----------------------------------------------------
    void   setStepValue(int step, int value); // write bits back to register
    void   setLock(int step, bool on);
    bool   isLocked(int step) const;
    ChordDef *tableSlot(int scale, int groupBits, int slot); // 2 or 4 bits

    // ---- accessors (live frame / display) --------------------------------
    uint16_t reg()        const { return reg_; }
    void     setReg(uint16_t r) { reg_ = r; }
    int      step()       const { return stepIdx_; }
    int      scale()      const { return scaleIdx_; }
    int      lengthIndex() const { return lengthIdx_; }
    int      length()     const { return kLengths[lengthIdx_]; }
    int      groupSize()  const { return length() <= 4 ? 4 : 2; }
    uint16_t lockMask()   const { return lockMask_; }
    void     setLockMask(uint16_t m) { lockMask_ = m; }
    bool     mutatedThisClock() const { return mutated_; }
    int      probabilityQ() const { return pQ_; } // 0..4096, for monitor

    // Tables are public-ish via accessor for persistence copy in/out.
    ChordDef table2_[kNumScales][k2BitTableLen];
    ChordDef table4_[kNumScales][k4BitTableLen];

private:
    uint32_t nextRand();

    uint16_t reg_        = 0xACE1;   // arbitrary non-zero seed pattern
    uint8_t  stepIdx_    = 0;
    uint8_t  scaleIdx_   = 0;
    uint8_t  lengthIdx_  = 4;        // default length = 8 (index 4)
    uint16_t lockMask_   = 0;
    int32_t  pQ_         = 0;        // mutation probability, 0..4096
    bool     hardLock_   = false;
    bool     doubleLen_  = false;    // CCW-half flag (see README: v1 deviation)
    bool     mutated_    = false;

    // tonic quantiser state
    int      tonicSemi_  = 0;        // current quantised semitone offset
    int      tonic_      = 0;        // current tonic MIDI note
    bool     tonicInit_  = false;

    uint32_t rng_ = 0x1234abcdu;
};

} // namespace chordloop

#endif // CHORDLOOP_CHORD_ENGINE_H
