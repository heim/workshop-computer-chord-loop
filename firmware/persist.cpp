// persist.cpp - pure serialisation, CRC and validation (host-tested).

#include "persist.h"
#include <cstring>

namespace chordloop {

// --- little-endian helpers -------------------------------------------------
static inline void putU16(uint8_t *&p, uint16_t v) { *p++ = v & 0xFF; *p++ = (v >> 8) & 0xFF; }
static inline void putU32(uint8_t *&p, uint32_t v) {
    *p++ = v & 0xFF; *p++ = (v >> 8) & 0xFF; *p++ = (v >> 16) & 0xFF; *p++ = (v >> 24) & 0xFF;
}
static inline uint16_t getU16(const uint8_t *&p) { uint16_t v = p[0] | (p[1] << 8); p += 2; return v; }
static inline uint32_t getU32(const uint8_t *&p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4; return v;
}

// --- CRC-32 (IEEE 802.3, poly 0xEDB88320, reflected) -----------------------
uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void putConfig(uint8_t *&p, const Config &c) {
    *p++ = c.chordClockDiv; *p++ = c.arpPatternOverride; *p++ = c.padEnabled; *p++ = c.padWaveform;
    *p++ = c.padDetuneCents; *p++ = c.padCutoff; *p++ = c.padWidth; *p++ = c.reserved;
}
static void getConfig(const uint8_t *&p, Config &c) {
    c.chordClockDiv = *p++; c.arpPatternOverride = *p++; c.padEnabled = *p++; c.padWaveform = *p++;
    c.padDetuneCents = *p++; c.padCutoff = *p++; c.padWidth = *p++; c.reserved = *p++;
}
static void putChord(uint8_t *&p, const ChordDef &d) {
    *p++ = d.degree; *p++ = d.type; *p++ = (uint8_t)d.inversion; *p++ = (uint8_t)d.octave;
}
static void getChord(const uint8_t *&p, ChordDef &d) {
    d.degree = *p++; d.type = *p++; d.inversion = (int8_t)*p++; d.octave = (int8_t)*p++;
}

size_t serializePersist(const PersistState &st, uint8_t *buf, size_t bufLen) {
    if (bufLen < kPersistBlobSize) return 0;
    uint8_t *p = buf;
    putU32(p, kPersistMagic);
    putU16(p, kPersistVersion);
    putU16(p, (uint16_t)kPersistBlobSize);
    putConfig(p, st.config);
    putU16(p, st.reg);
    putU16(p, st.lockMask);
    for (int s = 0; s < kNumScales; ++s)
        for (int i = 0; i < k2BitTableLen; ++i) putChord(p, st.table2[s][i]);
    for (int s = 0; s < kNumScales; ++s)
        for (int i = 0; i < k4BitTableLen; ++i) putChord(p, st.table4[s][i]);
    // CRC over everything written so far (header + payload), then append it.
    uint32_t crc = crc32(buf, (size_t)(p - buf));
    putU32(p, crc);
    return (size_t)(p - buf);
}

bool deserializePersist(const uint8_t *buf, size_t len, PersistState &st) {
    if (len < kPersistBlobSize) return false;
    const uint8_t *p = buf;
    if (getU32(p) != kPersistMagic) return false;
    uint16_t version = getU16(p);
    if (version != kPersistVersion) return false; // future: migrate instead
    uint16_t size = getU16(p);
    if (size != kPersistBlobSize) return false;

    // Verify CRC before trusting any field.
    uint32_t storedCrc = (uint32_t)buf[kPersistBlobSize - 4] |
                         ((uint32_t)buf[kPersistBlobSize - 3] << 8) |
                         ((uint32_t)buf[kPersistBlobSize - 2] << 16) |
                         ((uint32_t)buf[kPersistBlobSize - 1] << 24);
    if (crc32(buf, kPersistBlobSize - 4) != storedCrc) return false;

    getConfig(p, st.config);
    st.reg = getU16(p);
    st.lockMask = getU16(p);
    for (int s = 0; s < kNumScales; ++s)
        for (int i = 0; i < k2BitTableLen; ++i) getChord(p, st.table2[s][i]);
    for (int s = 0; s < kNumScales; ++s)
        for (int i = 0; i < k4BitTableLen; ++i) getChord(p, st.table4[s][i]);

    // Light sanity clamps so corrupt-but-CRC-valid data can't crash the engine.
    st.config.chordClockDiv = sanitizeClockDiv(st.config.chordClockDiv);
    if (st.config.arpPatternOverride > 5) st.config.arpPatternOverride = 0;
    if (st.config.padWaveform > 1) st.config.padWaveform = 0;
    return true;
}

void loadDefaultPersist(PersistState &st) {
    st.config   = defaultConfig();
    st.reg      = 0xACE1;
    st.lockMask = 0;
    for (int s = 0; s < kNumScales; ++s) {
        for (int i = 0; i < k2BitTableLen; ++i) st.table2[s][i] = kDefault2Bit[s][i];
        for (int i = 0; i < k4BitTableLen; ++i) st.table4[s][i] = kDefault4Bit[s][i];
    }
}

} // namespace chordloop
