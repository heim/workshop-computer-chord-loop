// serial_proto.h
//
// Line-delimited JSON protocol between the card and the WebSerial UI.
// This module is PURE (no SDK, no hardware): it parses a command line into a
// ParsedCommand and formats live frames / the full state dump into buffers.
// main.cpp/core1 owns the actual CDC I/O and cross-core plumbing.

#ifndef CHORDLOOP_SERIAL_PROTO_H
#define CHORDLOOP_SERIAL_PROTO_H

#include <cstdint>
#include <cstddef>
#include "music_tables.h"
#include "settings.h"

namespace chordloop {

enum CmdType : uint8_t {
    CMD_NONE = 0,
    CMD_HELLO,
    CMD_SETSTEP,
    CMD_LOCK,
    CMD_TABLE,
    CMD_CFG,
    CMD_SAVE,
    CMD_RANDOMIZE,
    CMD_RESET,
    CMD_LOADDEFAULTS,
};

struct ParsedCommand {
    CmdType type = CMD_NONE;

    // setStep / lock
    int  i = 0, v = 0;
    bool on = false;

    // table edit
    int scale = 0, mode = 0, slot = 0, deg = 0, ctype = 0, inv = 0, oct = 0;

    // cfg (only the present fields are applied)
    bool hasDiv=false, hasArp=false, hasPadEn=false, hasPadWave=false,
         hasPadDet=false, hasPadCut=false, hasPadWid=false, hasScale=false;
    int  div=0, arp=0, padEn=0, padWave=0, padDet=0, padCut=0, padWid=0, cfgScale=0;
};

// Parse one JSON line. Returns true if a recognised command was found.
bool parseCommand(const char *line, ParsedCommand &out);

// Compact live frame, ~10 Hz. Returns bytes written (excluding NUL), or 0.
struct LiveInfo {
    uint16_t reg;
    uint16_t lock;                 // per-step lock mask (card truth)
    int step, tonic, scale, len, bpmX10;
    bool mut;
};
int buildLiveFrame(char *buf, size_t n, const LiveInfo &li);

// Full state dump sent on "hello". Large; buffer should be >= 4096 bytes.
struct FullState {
    uint16_t version;
    Config   config;
    uint16_t reg, lockMask;
    int      scale, length, tonic; // length = ACTUAL step count (2..8)
    ChordDef table2[kNumScales][k2BitTableLen];
    ChordDef table4[kNumScales][k4BitTableLen];
};
int buildStateDump(char *buf, size_t n, const FullState &fs);

// ---- shared JSON field helpers (exposed for tests) -------------------------
bool jsonInt(const char *s, const char *key, int &out);
bool jsonBool(const char *s, const char *key, bool &out);
bool jsonStr(const char *s, const char *key, char *out, size_t n);

} // namespace chordloop

#endif // CHORDLOOP_SERIAL_PROTO_H
