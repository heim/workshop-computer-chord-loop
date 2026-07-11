// ChordLoop - a Turing-Machine-style looping chord progression generator for the
// Music Thing Modular Workshop System Computer (RP2040 program card).
//
// Built on Chris Johnson's header-only ComputerCard library (v0.3.x).
//
// A 16-bit shift register mutates under a probability knob (classic Turing
// Machine), but its bit-groups are decoded into diatonic chords via per-scale
// degree tables, so every mutation is a musical chord choice, never a wrong
// note. Two clock layers: a chord clock (incoming clock / N) advances the
// progression; an arp clock (every pulse) steps through the current chord.
//
// I/O map (see README for the full table):
//   Main knob : mutation probability (CW = locked, noon = chaos, CCW = locked)
//   Knob X    : progression length  {2,3,4,6,8}
//   Knob Y    : scale               {Major, Nat/Harm minor, modes, min pent}
//   Switch Up : hard lock (freeze the loop, overrides knob)
//   Switch Dn : (momentary, debounced) re-randomise the whole register
//   CV In 1   : tonic (1V/oct, quantised, updated on chord boundaries)
//   CV In 2   : arp pattern select (5 zones: up/down/updown/random/strum)
//   Pulse In 1: clock       Pulse In 2: reset progression to step 0
//   CV Out 1  : chord root/bass (1V/oct)   CV Out 2 : arpeggiator (1V/oct)
//   Pulse Out1: 10 ms trigger on chord change   Pulse Out2: 5 ms per arp step
//   Audio 1/2 : internal stereo pad voice
//   LEDs      : left column = 3-bit step counter; right column = chord/arp
//               pulses; all six flash 50 ms when a mutation flips bits
//
// Timing design (learned the hard way - see README "Notes"):
//   ProcessSample() runs in a 48 kHz ISR with a hard ~3000-cycle budget at
//   144 MHz. If it overruns, ComputerCard's ADC multiplexer sequencing degrades
//   and knob/switch/CV readings bleed into each other (symptoms: phantom
//   switch-down randomise, knobs stuck/reading mid-scale, tonic jumps). So:
//     * the whole binary is copied to RAM (no XIP stalls in the ISR),
//     * the ISR only ever copies a ~20-byte live snapshot to core1 - the big
//       table/config state lives on core1, which owns all edits anyway,
//     * the momentary-down randomise is level-debounced, and CV jack detection
//       must be stable for 50 ms before a CV input is trusted.
//
// USB control panel (WebSerial) is additive - the card is fully usable
// standalone. The entire USB stack (raw TinyUSB CDC) runs on core1; the 48 kHz
// audio ISR owns core0 and is never blocked. Cross-core data is lock-free:
//   core1 -> core0 : a single-producer/single-consumer command ring
//   core0 -> core1 : a seqlock'd compact live snapshot

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "ComputerCard.h"
#include "tusb.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

#include "chord_engine.h"
#include "arp.h"
#include "pad_voice.h"
#include "serial_proto.h"
#include "persist.h"

using namespace chordloop;

// Data-memory barrier. Cortex-M0+ is in-order, but this stops the compiler from
// reordering the value/index writes in the ring and seqlock. Portable so the
// host tests (which include none of this file) still model the intent.
static inline void mem_barrier() {
#if defined(__arm__) || defined(__ARM_ARCH)
    __asm volatile("dmb" ::: "memory");
#else
    __asm volatile("" ::: "memory");
#endif
}

static inline int clampMidi(int n) { return n < 0 ? 0 : (n > 127 ? 127 : n); }

// Clamp a cfg field into a valid Config. Shared by core0 (live config) and
// core1 (the persisted mirror) so both always hold identical, sane values.
static void sanitizeCfgField(Config &c, int field, int value);

// ===========================================================================
// Cross-core shared state
// ===========================================================================
namespace shared {

// --- core1 -> core0 : command ring (SPSC, lock-free) -----------------------
// The ring buffer itself is plain (non-volatile); ordering is provided by the
// volatile head/tail indices plus mem_barrier(). A single producer (core1) and
// single consumer (core0) make this safe without locks.
struct CmdMsg { uint8_t type; int16_t a, b, c, d, e, f, g; };
constexpr uint32_t kRingSize = 64;              // power of two
constexpr uint32_t kRingMask = kRingSize - 1;
CmdMsg            cmdRing[kRingSize];
volatile uint32_t cmdHead = 0;                  // producer (core1)
volatile uint32_t cmdTail = 0;                  // consumer (core0)

inline bool pushCmd(const CmdMsg &m) {
    uint32_t head = cmdHead;
    if ((head - cmdTail) >= kRingSize) return false; // full
    cmdRing[head & kRingMask] = m;
    mem_barrier();
    cmdHead = head + 1;                              // publish
    return true;
}
inline bool popCmd(CmdMsg &m) {
    uint32_t tail = cmdTail;
    if (tail == cmdHead) return false;               // empty
    mem_barrier();                                   // data was written before head advanced
    m = cmdRing[tail & kRingMask];
    mem_barrier();
    cmdTail = tail + 1;
    return true;
}

// --- core0 -> core1 : compact live snapshot (seqlock) ----------------------
// ~20 bytes. This is the ONLY state core0 publishes; everything bigger
// (degree tables, config) is owned by core1.
struct LiveSnap {
    uint16_t reg, lock;
    int16_t  step, tonic, scale, len, bpmX10;
    uint8_t  mut;
};
volatile uint32_t liveSeq = 0;
LiveSnap liveData = {};

inline void publishLive(const LiveSnap &s) {
    uint32_t v = liveSeq + 1u;
    liveSeq = v; mem_barrier();
    liveData = s;
    mem_barrier(); liveSeq = v + 1u;
}
inline bool readLive(LiveSnap &out, uint32_t &seqOut) {
    for (int i = 0; i < 16; ++i) {
        uint32_t s1 = liveSeq; if (s1 & 1u) continue;
        mem_barrier(); out = liveData; mem_barrier();
        if (liveSeq == s1) { seqOut = s1; return true; }
    }
    return false;
}

// command type ids carried in CmdMsg.type
enum { C_SETSTEP=1, C_LOCK, C_TABLE, C_CFG, C_RANDOMIZE, C_RESET, C_DEFAULTS };
// cfg field ids carried in CmdMsg.a for C_CFG
enum { CF_DIV=0, CF_ARP, CF_PADEN, CF_PADWAVE, CF_PADDET, CF_PADCUT, CF_PADWID };

} // namespace shared

static void sanitizeCfgField(Config &c, int field, int value) {
    switch (field) {
    case shared::CF_DIV:     c.chordClockDiv = sanitizeClockDiv((uint8_t)value); break;
    case shared::CF_ARP:     c.arpPatternOverride = (value < 0 || value > 5) ? 0 : (uint8_t)value; break;
    case shared::CF_PADEN:   c.padEnabled = value ? 1 : 0; break;
    case shared::CF_PADWAVE: c.padWaveform = value ? 1 : 0; break;
    case shared::CF_PADDET:  c.padDetuneCents = (uint8_t)(value < 0 ? 0 : (value > 50 ? 50 : value)); break;
    case shared::CF_PADCUT:  c.padCutoff = (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value)); break;
    case shared::CF_PADWID:  c.padWidth = (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value)); break;
    default: break;
    }
}

// Boot-time persisted state, loaded once on core0 in main() before core1
// starts; seeds both the card (core0) and the web/save mirror (core1).
static PersistState g_boot;

// ===========================================================================
// The card (core0)
// ===========================================================================
class ChordLoopCard : public ComputerCard {
public:
    explicit ChordLoopCard(const PersistState &boot) {
        uint64_t uid = UniqueCardID();
        engine_.init((uint32_t)(uid ^ (uid >> 32)) | 1u);
        arp_.init((uint32_t)(uid >> 16) | 1u);
        pad_.init();

        config_ = boot.config;
        engine_.setReg(boot.reg);
        engine_.setLockMask(boot.lockMask);
        for (int s = 0; s < kNumScales; ++s) {
            for (int i = 0; i < k2BitTableLen; ++i) engine_.table2_[s][i] = boot.table2[s][i];
            for (int i = 0; i < k4BitTableLen; ++i) engine_.table4_[s][i] = boot.table4[s][i];
        }
        applyPadParams();

        engine_.quantizeTonic(0);       // establish a tonic before the first clock
        curChord_ = engine_.currentChord();
        pad_.setChord(curChord_);
        publishLiveSnapshot();          // core1 reads a valid snapshot from its first loop
    }

    virtual void ProcessSample() override {
        ++animCounter_;
        if (samplesSinceClock_ < 0x3fffffff) ++samplesSinceClock_;
        if (settle_ > 0) --settle_;

        bool changed = drainCommands();

        // --- knobs (always live; zones settle without hysteresis at boot) ---
        engine_.setProbabilityKnob(KnobVal(Knob::Main));
        int li = zoneHyst(KnobVal(Knob::X), kNumLengths, 1);
        if (li != engine_.lengthIndex()) { engine_.setLengthIndex(li); changed = true; }
        int sc = zoneHyst(KnobVal(Knob::Y), kNumScales, 2);
        if (sc != engine_.scale()) { engine_.setScale(sc); changed = true; }

        // --- switch Z ---
        // Up = hard lock (level, ADC-smoothed upstream). Down = momentary
        // randomise, debounced: it must read Down for 10 ms straight before it
        // fires once, so a single glitched mux sample can never scramble the
        // register behind the user's back.
        Switch sw = SwitchVal();
        engine_.setHardLock(sw == Up);
        if (sw == Down) {
            if (downCount_ <= kDownDebounce) ++downCount_;
            if (downCount_ == kDownDebounce) { engine_.randomizeRegister(); changed = true; }
        } else {
            downCount_ = 0;
        }

        // --- reset ---
        if (PulseIn2RisingEdge()) { engine_.resetStep(); changed = true; }

        // --- CV jack stability: trust a CV input only after its jack has read
        // as connected for 50 ms straight (normalisation-probe flicker guard).
        cv1Stable_ = Connected(Input::CV1) ? (cv1Stable_ < 60000 ? cv1Stable_ + 1 : cv1Stable_) : 0;
        cv2Stable_ = Connected(Input::CV2) ? (cv2Stable_ < 60000 ? cv2Stable_ + 1 : cv2Stable_) : 0;

        // --- arp pattern selection ---
        int arpPat;
        if (config_.arpPatternOverride)      arpPat = config_.arpPatternOverride - 1;
        else if (cv2Stable_ >= kConnStable)  arpPat = Arp::patternFromCV(CVIn2());
        else                                 arpPat = ARP_UP;
        arp_.setPattern(arpPat);

        // --- clock ---
        if (PulseIn1RisingEdge()) {
            if (samplesSinceClock_ > 0 && samplesSinceClock_ < 4 * kSampleRate)
                clockPeriod_ = samplesSinceClock_;
            samplesSinceClock_ = 0;

            ++clockCounter_;
            uint8_t div = config_.chordClockDiv ? config_.chordClockDiv : 1;
            if (clockCounter_ % div == 0) {
                int tonicCV = (cv1Stable_ >= kConnStable) ? CVIn1() : 0;
                engine_.quantizeTonic(tonicCV);
                engine_.onChordClock();
                curChord_ = engine_.currentChord();
                arp_.onChordChange();
                pad_.setChord(curChord_);
                chordPulse_ = kChordPulseSamples;
                if (engine_.mutatedThisClock()) mutFlash_ = kMutationFlashSamples;
                CVOut1MIDINote((uint8_t)clampMidi(curChord_.bass));
            }

            int arpNote = arp_.step(curChord_);
            CVOut2MIDINote((uint8_t)clampMidi(arpNote));
            arpPulse_ = kArpPulseSamples;
            changed = true;
        }

        // --- internal pad voice ---
        if (config_.padEnabled) {
            int32_t l, r; pad_.render(l, r);
            AudioOut1((int16_t)l); AudioOut2((int16_t)r);
        } else {
            AudioOut1(0); AudioOut2(0);
        }

        // --- pulse outputs ---
        if (chordPulse_ > 0) { PulseOut1(true);  if (--chordPulse_ == 0) PulseOut1(false); }
        if (arpPulse_ > 0)   { PulseOut2(true);  if (--arpPulse_ == 0)   PulseOut2(false); }

        updateLeds(sw);

        // --- publish to core1: on any change, plus a ~10 ms heartbeat so the
        // web monitor tracks knob turns even between clocks.
        if (changed || (animCounter_ & 511) == 0) publishLiveSnapshot();
    }

private:
    static constexpr int kPickup       = 128;              // knob-zone hysteresis margin (of 4096)
    static constexpr int kDownDebounce = kSampleRate / 100; // 10 ms of solid Down
    static constexpr int kConnStable   = kSampleRate / 20;  // 50 ms of solid jack detect

    ChordEngine engine_;
    Arp         arp_;
    PadVoice    pad_;
    Config      config_;
    DecodedChord curChord_;

    int  clockCounter_ = 0;
    int  samplesSinceClock_ = 0x3fffffff;
    int  clockPeriod_ = 0;
    int  chordPulse_ = 0, arpPulse_ = 0, mutFlash_ = 0;
    int  downCount_ = 0;
    int  cv1Stable_ = 0, cv2Stable_ = 0;
    int  settle_ = kSampleRate / 2;   // 0.5 s: knob smoothing ramps from 0 at boot
    uint32_t animCounter_ = 0;
    int  lastZone_[3] = {-1, -1, -1};

    void applyPadParams() {
        pad_.setParams(config_.padWaveform, config_.padDetuneCents,
                       config_.padCutoff, config_.padWidth, config_.padEnabled != 0);
    }

    // Knob -> zone with a little hysteresis so a knob resting on a boundary does
    // not chatter between two scales / lengths. During the boot settle window
    // the zone simply tracks the (still-ramping) smoothed value.
    int zoneHyst(int32_t raw, int zones, int idx) {
        int val = (raw * zones) >> 12;
        if (val >= zones) val = zones - 1;
        if (val < 0) val = 0;
        if (settle_ > 0 || lastZone_[idx] < 0) { lastZone_[idx] = val; return val; }
        if (val != lastZone_[idx]) {
            int32_t zoneSize = 4096 / zones;
            int32_t center = lastZone_[idx] * zoneSize + zoneSize / 2;
            int32_t dist = raw - center; if (dist < 0) dist = -dist;
            if (dist > zoneSize / 2 + kPickup) lastZone_[idx] = val;
        }
        return lastZone_[idx];
    }

    bool drainCommands() {
        bool changed = false;
        shared::CmdMsg m;
        int guard = 0;
        while (shared::popCmd(m) && guard++ < 128) {
            switch (m.type) {
            case shared::C_SETSTEP: engine_.setStepValue(m.a, m.b); changed = true; break;
            case shared::C_LOCK:    engine_.setLock(m.a, m.b != 0); changed = true; break;
            case shared::C_TABLE: {
                ChordDef *slot = engine_.tableSlot(m.a, m.b, m.c);
                if (slot) {
                    slot->degree = (uint8_t)m.d; slot->type = (uint8_t)m.e;
                    slot->inversion = (int8_t)m.f; slot->octave = (int8_t)m.g;
                }
                changed = true;
                break;
            }
            case shared::C_CFG:
                sanitizeCfgField(config_, m.a, m.b);
                applyPadParams();
                changed = true;
                break;
            case shared::C_RANDOMIZE: engine_.randomizeRegister(); changed = true; break;
            case shared::C_RESET:     engine_.resetStep();         changed = true; break;
            case shared::C_DEFAULTS:  engine_.loadDefaultTables(); changed = true; break;
            default: break;
            }
        }
        return changed;
    }

    void updateLeds(Switch sw) {
        // Waiting animation when no clock for > 3 s.
        if (samplesSinceClock_ > kClockTimeoutSamples) {
            int pos = (animCounter_ >> 13) % 6;      // ~slow chase
            for (int i = 0; i < 6; ++i) LedOff(i);
            LedBrightness(pos, 900);
            return;
        }
        if (mutFlash_ > 0) {                          // "Turing struck" flash
            --mutFlash_;
            for (int i = 0; i < 6; ++i) LedOn(i);
            return;
        }
        int st = engine_.step();
        LedOn(kLedStepBit0, st & 1);
        LedOn(kLedStepBit1, st & 2);
        LedOn(kLedStepBit2, st & 4);
        LedOn(kLedChordPulse, chordPulse_ > 0);
        LedOn(kLedArpPulse,   arpPulse_ > 0);
        LedOn(kLedSpare,      sw == Up);              // lock indicator
    }

    void publishLiveSnapshot() {
        shared::LiveSnap s;
        s.reg    = engine_.reg();
        s.lock   = engine_.lockMask();
        s.step   = (int16_t)engine_.step();
        s.tonic  = (int16_t)engine_.tonic();
        s.scale  = (int16_t)engine_.scale();
        s.len    = (int16_t)engine_.length();
        s.bpmX10 = (int16_t)estimateBpmX10();
        s.mut    = engine_.mutatedThisClock() ? 1 : 0;
        shared::publishLive(s);
    }

    int estimateBpmX10() {
        if (clockPeriod_ <= 0) return 0;
        // Treat each incoming pulse as a beat: BPM = 60 * fs / period.
        long v = (60L * kSampleRate * 10L) / clockPeriod_;
        if (v > 30000) v = 30000;   // keep well inside int16 for the snapshot
        return (int)v;
    }
};

// ===========================================================================
// core1 - USB CDC (raw TinyUSB) + authoritative table/config mirror
// ===========================================================================

// core1's authoritative copy of tables + config. All edits arrive over USB on
// this core, so it can answer "hello" state dumps and build flash saves
// without ever asking the audio core for more than the tiny live snapshot.
// Seeded from g_boot before core1 launches.
static PersistState g_mirror;

static char     g_line[160];
static uint32_t g_lineLen = 0;
static bool     g_drop = false;
static uint32_t g_lastLiveSeq = 0;
static shared::LiveSnap g_lastLive;   // latest good snapshot (valid from boot)
static bool     g_helloPending = false;

// Push a command to core0, waiting briefly if the ring is full (it only fills
// while core0 is parked, e.g. during a flash save). Keeps USB serviced.
static void pushCmdBlocking(const shared::CmdMsg &m) {
    if (shared::pushCmd(m)) return;
    absolute_time_t deadline = make_timeout_time_ms(600);
    while (!shared::pushCmd(m)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return; // give up
        tud_task();
    }
}

// Refresh g_lastLive from the seqlock; returns true if a snapshot was read.
static bool refreshLive() {
    shared::LiveSnap s; uint32_t seq;
    if (!shared::readLive(s, seq)) return false;
    g_lastLive = s;
    g_lastLiveSeq = seq;
    return true;
}

// Write a possibly-large buffer to CDC, chunked, servicing USB between chunks so
// we never overflow the 256-byte TX FIFO or send a truncated line.
static void cdcWriteAll(const char *s, int len) {
    int sent = 0;
    int guard = 0;
    while (sent < len && guard++ < 20000) {
        tud_task();
        if (!tud_cdc_connected()) return;
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) { tud_cdc_write_flush(); continue; }
        int chunk = len - sent;
        if ((uint32_t)chunk > avail) chunk = (int)avail;
        tud_cdc_write(s + sent, (uint32_t)chunk);
        tud_cdc_write_flush();
        sent += chunk;
    }
}

// Assemble the full state dump from the core1 mirror + the live snapshot.
static void sendStateDump() {
    if (!tud_cdc_connected()) return;
    refreshLive(); // best effort; g_lastLive is valid since boot either way

    FullState fs;
    fs.version  = kPersistVersion;
    fs.config   = g_mirror.config;
    fs.reg      = g_lastLive.reg;
    fs.lockMask = g_lastLive.lock;
    fs.scale    = g_lastLive.scale;
    fs.length   = g_lastLive.len;      // ACTUAL progression length (2..8)
    fs.tonic    = g_lastLive.tonic;
    for (int s = 0; s < kNumScales; ++s) {
        for (int i = 0; i < k2BitTableLen; ++i) fs.table2[s][i] = g_mirror.table2[s][i];
        for (int i = 0; i < k4BitTableLen; ++i) fs.table4[s][i] = g_mirror.table4[s][i];
    }

    static char big[4608];
    int len = buildStateDump(big, sizeof(big), fs);
    if (len > 0) cdcWriteAll(big, len);
}

static void doSave() {
    refreshLive();
    PersistState ps = g_mirror;         // tables + config (authoritative here)
    ps.reg      = g_lastLive.reg;       // register + locks live on core0
    ps.lockMask = g_lastLive.lock;
    bool ok = savePersistToFlash(ps);   // parks core0 briefly (audio pause)
    char ack[48];
    int n = snprintf(ack, sizeof(ack), "{\"c\":\"saved\",\"ok\":%s}\n", ok ? "true" : "false");
    if (n > 0) cdcWriteAll(ack, n);
}

static void dispatch(const ParsedCommand &pc) {
    using namespace shared;
    switch (pc.type) {
    case CMD_HELLO: g_helloPending = true; break;
    case CMD_SAVE:  doSave(); break;
    case CMD_SETSTEP: { CmdMsg m{C_SETSTEP,(int16_t)pc.i,(int16_t)pc.v,0,0,0,0,0}; pushCmdBlocking(m); break; }
    case CMD_LOCK:    { CmdMsg m{C_LOCK,(int16_t)pc.i,(int16_t)(pc.on?1:0),0,0,0,0,0}; pushCmdBlocking(m); break; }
    case CMD_TABLE: {
        // Update the mirror (bounds-checked), then forward to the engine.
        if (pc.scale >= 0 && pc.scale < kNumScales) {
            ChordDef d = {(uint8_t)pc.deg, (uint8_t)pc.ctype, (int8_t)pc.inv, (int8_t)pc.oct};
            if (pc.mode == 2 && pc.slot >= 0 && pc.slot < k2BitTableLen)
                g_mirror.table2[pc.scale][pc.slot] = d;
            else if (pc.mode == 4 && pc.slot >= 0 && pc.slot < k4BitTableLen)
                g_mirror.table4[pc.scale][pc.slot] = d;
            else break;
            CmdMsg m{C_TABLE,(int16_t)pc.scale,(int16_t)pc.mode,(int16_t)pc.slot,
                     (int16_t)pc.deg,(int16_t)pc.ctype,(int16_t)pc.inv,(int16_t)pc.oct};
            pushCmdBlocking(m);
        }
        break;
    }
    case CMD_RANDOMIZE: { CmdMsg m{C_RANDOMIZE,0,0,0,0,0,0,0}; pushCmdBlocking(m); break; }
    case CMD_RESET:     { CmdMsg m{C_RESET,0,0,0,0,0,0,0}; pushCmdBlocking(m); break; }
    case CMD_LOADDEFAULTS: {
        for (int s = 0; s < kNumScales; ++s) {
            for (int i = 0; i < k2BitTableLen; ++i) g_mirror.table2[s][i] = kDefault2Bit[s][i];
            for (int i = 0; i < k4BitTableLen; ++i) g_mirror.table4[s][i] = kDefault4Bit[s][i];
        }
        CmdMsg m{C_DEFAULTS,0,0,0,0,0,0,0};
        pushCmdBlocking(m);
        break;
    }
    case CMD_CFG: {
        struct { bool has; int field; int val; } fields[] = {
            {pc.hasDiv,     CF_DIV,     pc.div},
            {pc.hasArp,     CF_ARP,     pc.arp},
            {pc.hasPadEn,   CF_PADEN,   pc.padEn},
            {pc.hasPadWave, CF_PADWAVE, pc.padWave},
            {pc.hasPadDet,  CF_PADDET,  pc.padDet},
            {pc.hasPadCut,  CF_PADCUT,  pc.padCut},
            {pc.hasPadWid,  CF_PADWID,  pc.padWid},
        };
        for (auto &f : fields) {
            if (!f.has) continue;
            sanitizeCfgField(g_mirror.config, f.field, f.val);
            CmdMsg m{C_CFG,(int16_t)f.field,(int16_t)f.val,0,0,0,0,0};
            pushCmdBlocking(m);
        }
        break;
    }
    default: break;
    }
}

static void handleLine(const char *line) {
    ParsedCommand pc;
    if (parseCommand(line, pc)) dispatch(pc);
}

static void pollCdcInput() {
    if (!tud_cdc_available()) return;
    uint8_t rx[64];
    uint32_t got = tud_cdc_read(rx, sizeof(rx));
    for (uint32_t i = 0; i < got; ++i) {
        char ch = (char)rx[i];
        if (ch == '\n' || ch == '\r') {
            if (!g_drop && g_lineLen > 0) { g_line[g_lineLen] = 0; handleLine(g_line); }
            g_lineLen = 0; g_drop = false;
        } else if (g_drop) {
            // discard until newline
        } else if (g_lineLen < sizeof(g_line) - 1) {
            g_line[g_lineLen++] = ch;
        } else {
            g_drop = true; g_lineLen = 0;
        }
    }
}

static void sendLive() {
    if (!tud_cdc_connected()) return;
    uint32_t prevSeq = g_lastLiveSeq;
    if (!refreshLive()) return;
    if (g_lastLiveSeq == prevSeq && prevSeq != 0) return;   // nothing new
    char buf[192];
    LiveInfo li;
    li.reg = g_lastLive.reg; li.lock = g_lastLive.lock;
    li.step = g_lastLive.step; li.tonic = g_lastLive.tonic;
    li.scale = g_lastLive.scale; li.len = g_lastLive.len;
    li.bpmX10 = g_lastLive.bpmX10; li.mut = g_lastLive.mut != 0;
    int len = buildLiveFrame(buf, sizeof(buf), li);
    if (len <= 0) return;
    if (tud_cdc_write_available() < (uint32_t)len) return;  // try again next round
    tud_cdc_write(buf, (uint32_t)len);
    tud_cdc_write_flush();
}

static void core1Main() {
    tud_init(BOARD_TUD_RHPORT);              // USB device stack lives on core1
    uint32_t lastPushMs = 0;
    const uint32_t kPushIntervalMs = 100;    // ~10 Hz live frames
    while (true) {
        tud_task();
        pollCdcInput();
        if (g_helloPending && tud_cdc_connected()) {
            g_helloPending = false;
            sendStateDump();
        }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((uint32_t)(now - lastPushMs) >= kPushIntervalMs) {
            sendLive();
            lastPushMs = now;
        }
    }
}

int main() {
    set_sys_clock_khz(144000, true);         // 3 x 48 MHz: low audio-input noise

    if (!loadPersistFromFlash(g_boot)) loadDefaultPersist(g_boot);
    g_mirror = g_boot;                       // seed core1's mirror before launch

    ChordLoopCard card(g_boot);              // constructed on core0 (flash/EEPROM/I2C)
    card.EnableNormalisationProbe();         // jack detection for CV1/CV2 defaults

    flash_safe_execute_core_init();          // register core0 as flash-lockout victim
    multicore_launch_core1(core1Main);       // USB on core1

    card.Run();                              // 48 kHz audio DSP on core0 (never returns)
    return 0;
}
