// persist.h
//
// Versioned, CRC-protected settings blob stored in the last flash sector.
//
// The serialisation / CRC / validation logic (persist.cpp) is pure and
// host-tested. The actual flash read/write (persist_flash.cpp) is only compiled
// into the firmware; on a CRC or version mismatch the caller falls back to
// compiled-in defaults.

#ifndef CHORDLOOP_PERSIST_H
#define CHORDLOOP_PERSIST_H

#include <cstdint>
#include <cstddef>
#include "music_tables.h"
#include "settings.h"

namespace chordloop {

// Everything that survives a power cycle.
struct PersistState {
    Config   config;
    uint16_t reg;
    uint16_t lockMask;
    ChordDef table2[kNumScales][k2BitTableLen];
    ChordDef table4[kNumScales][k4BitTableLen];
};

constexpr uint32_t kPersistMagic   = 0x43'4C'50'01u; // "CLP\x01"
constexpr uint16_t kPersistVersion = 1;

// header(8) + config(8) + reg(2) + lockMask(2) + table2(128) + table4(512) + crc(4)
constexpr size_t kPersistBlobSize =
    4 + 2 + 2 + 8 + 2 + 2 +
    (kNumScales * k2BitTableLen * 4) +
    (kNumScales * k4BitTableLen * 4) +
    4;

uint32_t crc32(const uint8_t *data, size_t len);

// Serialise `st` into `buf` (must be >= kPersistBlobSize). Returns bytes written,
// or 0 on insufficient buffer.
size_t serializePersist(const PersistState &st, uint8_t *buf, size_t bufLen);

// Validate + parse a blob into `st`. Returns true only if magic, version and CRC
// all check out.
bool deserializePersist(const uint8_t *buf, size_t len, PersistState &st);

// Fill `st` with compiled-in factory defaults.
void loadDefaultPersist(PersistState &st);

// ---- firmware-only flash I/O (defined in persist_flash.cpp) ----------------
// Returns true on success. Safe to call only from a non-audio context (core1);
// briefly parks the audio core while erasing/programming.
bool savePersistToFlash(const PersistState &st);
bool loadPersistFromFlash(PersistState &st);

} // namespace chordloop

#endif // CHORDLOOP_PERSIST_H
