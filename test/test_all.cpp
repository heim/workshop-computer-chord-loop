// test_all.cpp - host-side unit tests for ChordLoop's pure logic.
//
// Compiles the SDK-free firmware modules natively and exercises them. No test
// framework dependency: a tiny CHECK macro tallies pass/fail.
//
//   g++ -std=c++17 -I firmware test/test_all.cpp
//       firmware/chord_engine.cpp firmware/music_tables.cpp firmware/arp.cpp
//       firmware/persist.cpp firmware/serial_proto.cpp -o /tmp/tests && /tmp/tests

#include <cstdio>
#include <cstring>
#include <cstdint>

#include "chord_engine.h"
#include "arp.h"
#include "persist.h"
#include "serial_proto.h"
#include "music_tables.h"

using namespace chordloop;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

// ---------------------------------------------------------------------------
// Register mutation: p=0 locked, p=1 mutates, lockMask respected
// ---------------------------------------------------------------------------
static void test_register() {
    std::printf("[register]\n");

    // p = 0 (knob fully CW): the register must never change over 10k clocks.
    {
        ChordEngine e; e.init(12345);
        e.setLengthIndex(4);            // length 8, 2-bit groups
        e.setProbabilityKnob(4095);     // fully CW -> locked
        uint16_t start = e.reg();
        bool changed = false;
        for (int i = 0; i < 10000; ++i) { e.onChordClock(); if (e.reg() != start) changed = true; }
        CHECK(!changed, "p=0 keeps register locked over 10k clocks");
    }

    // p = 0 fully CCW is also locked.
    {
        ChordEngine e; e.init(999);
        e.setProbabilityKnob(0);
        uint16_t start = e.reg();
        bool changed = false;
        for (int i = 0; i < 10000; ++i) { e.onChordClock(); if (e.reg() != start) changed = true; }
        CHECK(!changed, "fully CCW keeps register locked");
    }

    // p = 1 (noon): the register must mutate.
    {
        ChordEngine e; e.init(2024);
        e.setLengthIndex(4);
        e.setProbabilityKnob(2048);     // noon -> max chaos
        uint16_t start = e.reg();
        bool changed = false;
        for (int i = 0; i < 200; ++i) { e.onChordClock(); if (e.reg() != start) { changed = true; break; } }
        CHECK(changed, "p=1 mutates the register");
    }

    // Hard lock overrides a chaotic knob.
    {
        ChordEngine e; e.init(7);
        e.setProbabilityKnob(2048);
        e.setHardLock(true);
        uint16_t start = e.reg();
        bool changed = false;
        for (int i = 0; i < 5000; ++i) { e.onChordClock(); if (e.reg() != start) changed = true; }
        CHECK(!changed, "hard lock freezes the register even at p=1");
    }

    // lockMask: a locked step's bits are never flipped.
    {
        ChordEngine e; e.init(555);
        e.setLengthIndex(4);            // length 8, group size 2 => step s owns bits [2s,2s+1]
        e.setProbabilityKnob(2048);
        for (int s = 0; s < 8; ++s) e.setLock(s, false);
        e.setLock(3, true);
        uint16_t mask = 0x3u << (3 * 2); // bits owned by step 3
        uint16_t lockedBits = e.reg() & mask;
        bool violated = false;
        for (int i = 0; i < 20000; ++i) {
            e.onChordClock();
            if ((e.reg() & mask) != lockedBits) violated = true;
        }
        CHECK(!violated, "locked step's bits are never mutated at p=1");
    }
}

// ---------------------------------------------------------------------------
// Decode: every default-table slot yields only in-scale notes, all 12 tonics
// ---------------------------------------------------------------------------
static bool inScale(int scale, int noteMinusTonic) {
    int pc = ((noteMinusTonic % 12) + 12) % 12;
    int len = scaleLength(scale);
    for (int i = 0; i < len; ++i) {
        int iv = ((kScaleIntervals[scale][i] % 12) + 12) % 12;
        if (iv == pc) return true;
    }
    return false;
}

static void test_decode_in_scale() {
    std::printf("[decode in-scale]\n");
    int bad = 0, total = 0;
    for (int scale = 0; scale < kNumScales; ++scale) {
        for (int tonic = 54; tonic <= 65; ++tonic) { // 12 consecutive tonics
            // 2-bit table
            for (int slot = 0; slot < k2BitTableLen; ++slot) {
                DecodedChord dc = decodeChord(scale, kDefault2Bit[scale][slot], tonic);
                for (int j = 0; j < dc.count; ++j) {
                    ++total;
                    if (!inScale(scale, dc.notes[j] - tonic)) ++bad;
                }
            }
            // 4-bit table
            for (int slot = 0; slot < k4BitTableLen; ++slot) {
                DecodedChord dc = decodeChord(scale, kDefault4Bit[scale][slot], tonic);
                for (int j = 0; j < dc.count; ++j) {
                    ++total;
                    if (!inScale(scale, dc.notes[j] - tonic)) ++bad;
                }
            }
        }
    }
    std::printf("  checked %d chord tones across all scales/tonics\n", total);
    CHECK(bad == 0, "every default-table chord tone is in-scale for all 12 tonics");
}

static void test_decode_shape() {
    std::printf("[decode shape]\n");
    // C major, tonic 60: I triad = C E G = 60,64,67
    ChordDef I = {0, TYPE_TRIAD, 0, 0};
    DecodedChord dc = decodeChord(SCALE_MAJOR, I, 60);
    CHECK(dc.count == 3, "major triad has 3 tones");
    CHECK(dc.notes[0] == 60 && dc.notes[1] == 64 && dc.notes[2] == 67, "C major I = C E G");
    CHECK(dc.bass == 60, "root position bass is the root");

    // First inversion: bass becomes E (64)
    ChordDef I1 = {0, TYPE_TRIAD, 1, 0};
    DecodedChord dci = decodeChord(SCALE_MAJOR, I1, 60);
    CHECK(dci.bass == 64, "first inversion bass is the third");
    CHECK(dci.root == 60, "root reference stays the root under inversion");

    // V7 in C major = G B D F = 67,71,74,77
    ChordDef V7 = {4, TYPE_SEVENTH, 0, 0};
    DecodedChord d7 = decodeChord(SCALE_MAJOR, V7, 60);
    CHECK(d7.count == 4, "seventh chord has 4 tones");
    CHECK(d7.notes[0] == 67 && d7.notes[3] == 77, "G7 spans G..F");

    // octave shift
    ChordDef Iup = {0, TYPE_TRIAD, 0, 1};
    DecodedChord du = decodeChord(SCALE_MAJOR, Iup, 60);
    CHECK(du.notes[0] == 72, "octave +1 raises the chord an octave");
}

// ---------------------------------------------------------------------------
// Tonic quantiser hysteresis
// ---------------------------------------------------------------------------
static void test_tonic_hysteresis() {
    std::printf("[tonic hysteresis]\n");
    ChordEngine e; e.init(1);

    int center = 0; // 0 V -> base tonic
    int t0 = e.quantizeTonic(center);
    CHECK(t0 == kTonicBaseMidi, "0V quantises to base tonic");

    // small noise within the dead-band must not change the tonic
    bool moved = false;
    for (int n = -kCvCountsPerSemi / 2; n <= kCvCountsPerSemi / 2; ++n) {
        if (e.quantizeTonic(n) != t0) moved = true;
    }
    CHECK(!moved, "sub-semitone noise does not change the tonic");

    // a clear move up by 2 semitones (centre of the +2 note) changes it
    int t2 = e.quantizeTonic(2 * kCvCountsPerVolt / 12);
    CHECK(t2 == kTonicBaseMidi + 2, "a clear +2 semitone input shifts the tonic by 2");

    // one octave up
    ChordEngine e2; e2.init(1);
    int oct = e2.quantizeTonic(kCvCountsPerVolt);
    CHECK(oct == kTonicBaseMidi + 12, "+1V input shifts the tonic by an octave");
}

// ---------------------------------------------------------------------------
// Persistence round-trip + CRC
// ---------------------------------------------------------------------------
static void test_persist() {
    std::printf("[persist]\n");

    PersistState a;
    loadDefaultPersist(a);
    // perturb some fields
    a.config.chordClockDiv = 16;
    a.config.padDetuneCents = 9;
    a.reg = 0xBEEF;
    a.lockMask = 0x00A5;
    a.table4[2][7] = {3, TYPE_SUS4, 1, -1};
    a.table2[5][1] = {6, TYPE_POWER, 0, 1};

    uint8_t buf[kPersistBlobSize + 16];
    size_t n = serializePersist(a, buf, sizeof(buf));
    CHECK(n == kPersistBlobSize, "serialize writes the expected blob size");

    PersistState b;
    bool ok = deserializePersist(buf, n, b);
    CHECK(ok, "deserialize accepts a freshly serialized blob");
    CHECK(b.reg == a.reg && b.lockMask == a.lockMask, "reg/lockMask round-trip");
    CHECK(b.config.chordClockDiv == 16 && b.config.padDetuneCents == 9, "config round-trips");
    CHECK(memcmp(b.table4, a.table4, sizeof(a.table4)) == 0, "4-bit tables round-trip");
    CHECK(memcmp(b.table2, a.table2, sizeof(a.table2)) == 0, "2-bit tables round-trip");

    // CRC rejects a single-bit corruption
    buf[40] ^= 0x01;
    PersistState c;
    CHECK(!deserializePersist(buf, n, c), "CRC rejects a corrupted blob");
    buf[40] ^= 0x01; // restore

    // bad magic rejected
    uint8_t save0 = buf[0]; buf[0] ^= 0xFF;
    CHECK(!deserializePersist(buf, n, c), "wrong magic rejected");
    buf[0] = save0;

    // too-short buffer rejected
    CHECK(!deserializePersist(buf, kPersistBlobSize - 1, c), "short buffer rejected");
}

// ---------------------------------------------------------------------------
// Arpeggiator
// ---------------------------------------------------------------------------
static void test_arp() {
    std::printf("[arp]\n");
    DecodedChord ch;
    ch.count = 3; ch.notes[0] = 60; ch.notes[1] = 64; ch.notes[2] = 67; ch.bass = 60; ch.root = 60;

    { // UP cycles ascending
        Arp a; a.init(1); a.setPattern(ARP_UP);
        int seq[6]; for (int i = 0; i < 6; ++i) seq[i] = a.step(ch);
        CHECK(seq[0]==60 && seq[1]==64 && seq[2]==67 && seq[3]==60 && seq[4]==64 && seq[5]==67,
              "UP cycles 60,64,67,...");
    }
    { // DOWN cycles descending
        Arp a; a.init(1); a.setPattern(ARP_DOWN);
        int s0=a.step(ch), s1=a.step(ch), s2=a.step(ch), s3=a.step(ch);
        CHECK(s0==67 && s1==64 && s2==60 && s3==67, "DOWN cycles 67,64,60,...");
    }
    { // UPDOWN bounces without repeating the turning points
        Arp a; a.init(1); a.setPattern(ARP_UPDOWN);
        int s[5]; for (int i=0;i<5;++i) s[i]=a.step(ch);
        CHECK(s[0]==60 && s[1]==64 && s[2]==67 && s[3]==64 && s[4]==60, "UPDOWN bounces 60,64,67,64,60");
    }
    { // STRUM plays all tones then holds the root
        Arp a; a.init(1); a.setPattern(ARP_STRUM); a.onChordChange();
        int s0=a.step(ch), s1=a.step(ch), s2=a.step(ch), s3=a.step(ch), s4=a.step(ch);
        CHECK(s0==60 && s1==64 && s2==67 && s3==60 && s4==60, "STRUM: tones then hold root");
    }
    { // RANDOM stays within the chord tones
        Arp a; a.init(42); a.setPattern(ARP_RANDOM);
        bool inSet = true;
        for (int i=0;i<200;++i){ int n=a.step(ch); if(n!=60&&n!=64&&n!=67) inSet=false; }
        CHECK(inSet, "RANDOM only emits chord tones");
    }
    { // CV zones map across the range to the 5 patterns
        CHECK(Arp::patternFromCV(-2048)==ARP_UP,   "CV low -> UP");
        CHECK(Arp::patternFromCV(2047)==ARP_STRUM, "CV high -> STRUM");
        CHECK(Arp::patternFromCV(0)>=0 && Arp::patternFromCV(0)<kNumArpPatterns, "CV mid -> valid pattern");
    }
}

// ---------------------------------------------------------------------------
// Serial protocol
// ---------------------------------------------------------------------------
static void test_serial() {
    std::printf("[serial]\n");
    ParsedCommand pc;

    CHECK(parseCommand("{\"c\":\"hello\"}", pc) && pc.type == CMD_HELLO, "parse hello");

    CHECK(parseCommand("{\"c\":\"setStep\",\"i\":2,\"v\":5}", pc) &&
          pc.type == CMD_SETSTEP && pc.i == 2 && pc.v == 5, "parse setStep");

    CHECK(parseCommand("{\"c\":\"lock\",\"i\":3,\"on\":true}", pc) &&
          pc.type == CMD_LOCK && pc.i == 3 && pc.on == true, "parse lock true");
    CHECK(parseCommand("{\"c\":\"lock\",\"i\":3,\"on\":false}", pc) && pc.on == false, "parse lock false");

    CHECK(parseCommand("{\"c\":\"table\",\"scale\":0,\"mode\":4,\"slot\":13,\"deg\":1,\"type\":3,\"inv\":0,\"oct\":-1}", pc) &&
          pc.type == CMD_TABLE && pc.scale == 0 && pc.mode == 4 && pc.slot == 13 &&
          pc.deg == 1 && pc.ctype == 3 && pc.inv == 0 && pc.oct == -1, "parse table w/ negative oct");

    CHECK(parseCommand("{\"c\":\"cfg\",\"div\":8}", pc) && pc.type == CMD_CFG &&
          pc.hasDiv && pc.div == 8 && !pc.hasArp, "parse cfg div only");

    CHECK(parseCommand("{\"c\":\"save\"}", pc) && pc.type == CMD_SAVE, "parse save");
    CHECK(!parseCommand("not json", pc), "reject non-JSON");
    CHECK(!parseCommand("{\"c\":\"bogus\"}", pc), "reject unknown command");

    // live frame builds and is well-formed
    char buf[160];
    LiveInfo li; li.reg = 43690; li.step = 3; li.tonic = 60; li.scale = 0; li.len = 8; li.bpmX10 = 1204; li.mut = false;
    int n = buildLiveFrame(buf, sizeof(buf), li);
    CHECK(n > 0, "live frame builds");
    CHECK(strstr(buf, "\"reg\":43690") && strstr(buf, "\"bpm\":120.4") && strstr(buf, "\"mut\":false"),
          "live frame contains expected fields");
}

int main() {
    std::printf("ChordLoop host tests\n====================\n");
    test_register();
    test_decode_in_scale();
    test_decode_shape();
    test_tonic_hysteresis();
    test_persist();
    test_arp();
    test_serial();
    std::printf("====================\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
