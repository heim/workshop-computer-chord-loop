// arp.h
//
// Arpeggiator state machine. Pure logic (no SDK), unit-tested on the host.
// Given the current voiced chord, step() advances one arp step and returns the
// MIDI note to send to CV out 2. Called on every incoming clock pulse.

#ifndef CHORDLOOP_ARP_H
#define CHORDLOOP_ARP_H

#include <cstdint>
#include "chord_engine.h" // DecodedChord

namespace chordloop {

enum ArpPattern : uint8_t {
    ARP_UP = 0,
    ARP_DOWN,
    ARP_UPDOWN,
    ARP_RANDOM,
    ARP_STRUM,
};
constexpr int kNumArpPatterns = 5;

class Arp {
public:
    void init(uint32_t seed);
    void setPattern(int p);         // clamps to a valid ArpPattern
    int  pattern() const { return pattern_; }

    void onChordChange();           // re-anchor the strum on a new chord
    int  step(const DecodedChord &chord); // advance, return MIDI note
    int  lastNote() const { return lastNote_; }

    // Map a CV-in-2 count (-2048..2047) to one of the 5 patterns (5 zones).
    static int patternFromCV(int cvCounts);

private:
    uint32_t nextRand();

    uint8_t  pattern_  = ARP_UP;
    int      pos_      = 0;   // shared index for up/down/updown
    int      dir_      = 1;   // updown direction
    int      strumPos_ = 0;   // strum progress
    int      lastNote_ = 60;
    uint32_t rng_      = 0x9e3779b9u;
};

} // namespace chordloop

#endif // CHORDLOOP_ARP_H
