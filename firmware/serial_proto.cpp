// serial_proto.cpp - JSON parsing/formatting for the WebSerial protocol.

#include "serial_proto.h"
#include <cstring>
#include <cstdio>

namespace chordloop {

// Locate `"key"` and return a pointer just past the value's colon, skipping
// whitespace. Returns nullptr if not found.
static const char *afterKey(const char *s, const char *key) {
    char pat[40];
    int kn = (int)strlen(key);
    if (kn > 36) return nullptr;
    pat[0] = '"';
    memcpy(pat + 1, key, kn);
    pat[1 + kn] = '"';
    pat[2 + kn] = 0;
    const char *p = strstr(s, pat);
    if (!p) return nullptr;
    p += kn + 2;                 // past closing quote
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != ':') return nullptr;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

bool jsonInt(const char *s, const char *key, int &out) {
    const char *p = afterKey(s, key);
    if (!p) return false;
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    if (*p < '0' || *p > '9') return false;
    long val = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); ++p; if (val > 100000000L) break; }
    out = (int)(neg ? -val : val);
    return true;
}

bool jsonBool(const char *s, const char *key, bool &out) {
    const char *p = afterKey(s, key);
    if (!p) return false;
    if (strncmp(p, "true", 4) == 0)  { out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { out = false; return true; }
    if (*p == '1') { out = true;  return true; }
    if (*p == '0') { out = false; return true; }
    return false;
}

bool jsonStr(const char *s, const char *key, char *out, size_t n) {
    const char *p = afterKey(s, key);
    if (!p || *p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return true;
}

bool parseCommand(const char *line, ParsedCommand &out) {
    out = ParsedCommand();
    char c[24];
    if (!jsonStr(line, "c", c, sizeof(c))) return false;

    if      (strcmp(c, "hello") == 0)        out.type = CMD_HELLO;
    else if (strcmp(c, "save") == 0)         out.type = CMD_SAVE;
    else if (strcmp(c, "randomize") == 0)    out.type = CMD_RANDOMIZE;
    else if (strcmp(c, "reset") == 0)        out.type = CMD_RESET;
    else if (strcmp(c, "defaults") == 0)     out.type = CMD_LOADDEFAULTS;
    else if (strcmp(c, "setStep") == 0) {
        out.type = CMD_SETSTEP;
        jsonInt(line, "i", out.i);
        jsonInt(line, "v", out.v);
    }
    else if (strcmp(c, "lock") == 0) {
        out.type = CMD_LOCK;
        jsonInt(line, "i", out.i);
        jsonBool(line, "on", out.on);
    }
    else if (strcmp(c, "table") == 0) {
        out.type = CMD_TABLE;
        jsonInt(line, "scale", out.scale);
        jsonInt(line, "mode",  out.mode);
        jsonInt(line, "slot",  out.slot);
        jsonInt(line, "deg",   out.deg);
        jsonInt(line, "type",  out.ctype);
        jsonInt(line, "inv",   out.inv);
        jsonInt(line, "oct",   out.oct);
    }
    else if (strcmp(c, "cfg") == 0) {
        out.type = CMD_CFG;
        out.hasDiv     = jsonInt(line, "div",     out.div);
        out.hasArp     = jsonInt(line, "arp",     out.arp);
        out.hasPadEn   = jsonInt(line, "padEn",   out.padEn);
        out.hasPadWave = jsonInt(line, "padWave", out.padWave);
        out.hasPadDet  = jsonInt(line, "padDet",  out.padDet);
        out.hasPadCut  = jsonInt(line, "padCut",  out.padCut);
        out.hasPadWid  = jsonInt(line, "padWid",  out.padWid);
        out.hasScale   = jsonInt(line, "scale",   out.cfgScale);
    }
    else return false;

    return true;
}

int buildLiveFrame(char *buf, size_t n, const LiveInfo &li) {
    int len = snprintf(buf, n,
        "{\"c\":\"live\",\"reg\":%u,\"lock\":%u,\"step\":%d,\"tonic\":%d,\"scale\":%d,"
        "\"len\":%d,\"bpm\":%d.%d,\"mut\":%s}\n",
        (unsigned)li.reg, (unsigned)li.lock, li.step, li.tonic, li.scale, li.len,
        li.bpmX10 / 10, li.bpmX10 % 10, li.mut ? "true" : "false");
    if (len < 0 || (size_t)len >= n) return 0;
    return len;
}

static int appendChord(char *buf, size_t n, size_t off, const ChordDef &d) {
    int w = snprintf(buf + off, n - off, "[%u,%u,%d,%d]",
                     d.degree, d.type, d.inversion, d.octave);
    return (w < 0) ? -1 : w;
}

int buildStateDump(char *buf, size_t n, const FullState &fs) {
    size_t off = 0;
    int w = snprintf(buf, n,
        "{\"c\":\"state\",\"ver\":%u,\"scale\":%d,\"len\":%d,\"reg\":%u,"
        "\"lock\":%u,\"tonic\":%d,"
        "\"cfg\":{\"div\":%u,\"arp\":%u,\"padEn\":%u,\"padWave\":%u,"
        "\"padDet\":%u,\"padCut\":%u,\"padWid\":%u},",
        fs.version, fs.scale, fs.length, (unsigned)fs.reg,
        (unsigned)fs.lockMask, fs.tonic,
        fs.config.chordClockDiv, fs.config.arpPatternOverride,
        fs.config.padEnabled, fs.config.padWaveform,
        fs.config.padDetuneCents, fs.config.padCutoff, fs.config.padWidth);
    if (w < 0 || (size_t)w >= n) return 0;
    off = (size_t)w;

    // t2 / t4 as nested arrays: [scale][slot][deg,type,inv,oct]
    const char *tags[2] = {"\"t2\":[", "\"t4\":["};
    int lens[2] = {k2BitTableLen, k4BitTableLen};
    for (int t = 0; t < 2; ++t) {
        w = snprintf(buf + off, n - off, "%s", tags[t]);
        if (w < 0 || off + (size_t)w >= n) return 0;
        off += w;
        for (int s = 0; s < kNumScales; ++s) {
            if (off + 2 >= n) return 0;
            buf[off++] = '[';
            for (int i = 0; i < lens[t]; ++i) {
                const ChordDef &d = (t == 0) ? fs.table2[s][i] : fs.table4[s][i];
                w = appendChord(buf, n, off, d);
                if (w < 0 || off + (size_t)w >= n) return 0;
                off += w;
                if (i + 1 < lens[t]) { if (off + 1 >= n) return 0; buf[off++] = ','; }
            }
            if (off + 1 >= n) return 0;
            buf[off++] = ']';
            if (s + 1 < kNumScales) { if (off + 1 >= n) return 0; buf[off++] = ','; }
        }
        if (off + 2 >= n) return 0;
        buf[off++] = ']';
        buf[off++] = (t == 0) ? ',' : '}';
    }
    if (off + 2 >= n) return 0;
    buf[off++] = '\n';
    buf[off] = 0;
    return (int)off;
}

} // namespace chordloop
