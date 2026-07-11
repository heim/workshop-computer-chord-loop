// arp.cpp - arpeggiator implementation.

#include "arp.h"

namespace chordloop {

void Arp::init(uint32_t seed) {
    rng_      = seed ? seed : 0x9e3779b9u;
    pattern_  = ARP_UP;
    pos_      = 0;
    dir_      = 1;
    strumPos_ = 0;
    lastNote_ = 60;
}

uint32_t Arp::nextRand() {
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
}

void Arp::setPattern(int p) {
    if (p < 0) p = 0;
    if (p >= kNumArpPatterns) p = kNumArpPatterns - 1;
    if (p != pattern_) {
        pattern_  = static_cast<uint8_t>(p);
        pos_      = 0;
        dir_      = 1;
        strumPos_ = 0;
    }
}

void Arp::onChordChange() {
    // Restart the strum from the bottom of the new chord. Up/Down/UpDown keep
    // flowing (their index is clamped at read time).
    strumPos_ = 0;
}

int Arp::step(const DecodedChord &chord) {
    int n = chord.count;
    if (n <= 0) return lastNote_;

    int note;
    switch (pattern_) {
    case ARP_DOWN:
        if (pos_ < 0 || pos_ >= n) pos_ = 0;
        note = chord.notes[n - 1 - pos_];
        pos_ = (pos_ + 1) % n;
        break;

    case ARP_UPDOWN:
        if (pos_ >= n) pos_ = n - 1;
        if (pos_ < 0)  pos_ = 0;
        note = chord.notes[pos_];
        if (n > 1) {
            pos_ += dir_;
            if (pos_ >= n - 1) { pos_ = n - 1; dir_ = -1; }
            else if (pos_ <= 0) { pos_ = 0; dir_ = 1; }
        }
        break;

    case ARP_RANDOM:
        pos_ = static_cast<int>(nextRand() % static_cast<uint32_t>(n));
        note = chord.notes[pos_];
        break;

    case ARP_STRUM:
        if (strumPos_ < n) { note = chord.notes[strumPos_]; ++strumPos_; }
        else               { note = chord.notes[0]; } // hold root/bass
        break;

    case ARP_UP:
    default:
        if (pos_ < 0 || pos_ >= n) pos_ = 0;
        note = chord.notes[pos_];
        pos_ = (pos_ + 1) % n;
        break;
    }

    lastNote_ = note;
    return note;
}

int Arp::patternFromCV(int cvCounts) {
    // Full input span is ~ -2048..2047 (4096 counts) split into 5 zones.
    int z = (cvCounts + 2048) * kNumArpPatterns / 4096;
    if (z < 0) z = 0;
    if (z >= kNumArpPatterns) z = kNumArpPatterns - 1;
    return z;
}

} // namespace chordloop
