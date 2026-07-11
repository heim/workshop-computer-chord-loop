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
//   Switch Dn : (momentary) re-randomise the whole register
//   CV In 1   : tonic (1V/oct, quantised, updated on chord boundaries)
//   CV In 2   : arp pattern select (5 zones: up/down/updown/random/strum)
//   Pulse In 1: clock       Pulse In 2: reset progression to step 0
//   CV Out 1  : chord root/bass (1V/oct)   CV Out 2 : arpeggiator (1V/oct)
//   Pulse Out1: 10 ms trigger on chord change   Pulse Out2: 5 ms per arp step
//   Audio 1/2 : internal stereo pad voice
//   LEDs      : left column = 3-bit step counter; right column = chord/arp
//               pulses; all six flash 50 ms when a mutation flips bits
//
// USB control panel (WebSerial) is additive - the card is fully usable
// standalone. The entire USB stack (raw TinyUSB CDC) runs on core1; the 48 kHz
// audio ISR owns core0 and is never blocked. Cross-core data is lock-free:
//   core1 -> core0 : a single-producer/single-consumer command ring
//   core0 -> core1 : seqlock snapshots (a compact live frame + a full state)

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
// reordering the value/index writes in the ring and seqlocks. Portable so the
// host tests (which include none of this file) still model the intent.
static inline void mem_barrier() {
#if defined(__arm__) || defined(__ARM_ARCH)
    __asm volatile("dmb" ::: "memory");
#else
    __asm volatile("" ::: "memory");
#endif
}

static inline int clampMidi(int n) { return n < 0 ? 0 : (n > 127 ? 127 : n); }

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
struct LiveSnap { uint16_t reg; int16_t step, tonic, scale, len, bpmX10; uint8_t mut; };
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

// --- core0 -> core1 : full state snapshot (seqlock, published on edits) -----
volatile uint32_t fullSeq = 0;
FullState fullData = {};

inline void publishFull(const FullState &s) {
    uint32_t v = fullSeq + 1u;
    fullSeq = v; mem_barrier();
    fullData = s;
    mem_barrier(); fullSeq = v + 1u;
}
inline bool readFull(FullState &out, uint32_t &seqOut) {
    for (int i = 0; i < 8; ++i) {
        uint32_t s1 = fullSeq; if (s1 & 1u) continue;
        mem_barrier(); out = fullData; mem_barrier();
        if (fullSeq == s1) { seqOut = s1; return true; }
    }
    return false;
}

// command type ids carried in CmdMsg.type
enum { C_SETSTEP=1, C_LOCK, C_TABLE, C_CFG, C_RANDOMIZE, C_RESET, C_DEFAULTS };
// cfg field ids carried in CmdMsg.a for C_CFG
enum { CF_DIV=0, CF_ARP, CF_PADEN, CF_PADWAVE, CF_PADDET, CF_PADCUT, CF_PADWID };

} // namespace shared

// ===========================================================================
// The card
// ===========================================================================
class ChordLoopCard : public ComputerCard {
public:
    ChordLoopCard() {
        uint64_t uid = UniqueCardID();
        engine_.init((uint32_t)(uid ^ (uid >> 32)) | 1u);
        arp_.init((uint32_t)(uid >> 16) | 1u);
        pad_.init();

        // Load persisted settings, else compiled-in defaults.
        PersistState ps;
        if (!loadPersistFromFlash(ps)) loadDefaultPersist(ps);
        config_ = ps.config;
        engine_.setReg(ps.reg);
        engine_.setLockMask(ps.lockMask);
        for (int s = 0; s < kNumScales; ++s) {
            for (int i = 0; i < k2BitTableLen; ++i) engine_.table2_[s][i] = ps.table2[s][i];
            for (int i = 0; i < k4BitTableLen; ++i) engine_.table4_[s][i] = ps.table4[s][i];
        }
        applyPadParams();

        engine_.quantizeTonic(0);       // establish a tonic before the first clock
        curChord_ = engine_.currentChord();
        pad_.setChord(curChord_);
        publishFullSnapshot();
    }

    virtual void ProcessSample() override {
        ++animCounter_;
        if (samplesSinceClock_ < 0x3fffffff) ++samplesSinceClock_;

        bool structural = drainCommands();

        // --- knobs (always live) ---
        engine_.setProbabilityKnob(KnobVal(Knob::Main));
        int li = zoneHyst(KnobVal(Knob::X), kNumLengths, 1);
        if (li != engine_.lengthIndex()) { engine_.setLengthIndex(li); structural = true; }
        int sc = zoneHyst(KnobVal(Knob::Y), kNumScales, 2);
        if (sc != engine_.scale()) { engine_.setScale(sc); structural = true; }

        // --- switch Z ---
        Switch sw = SwitchVal();
        engine_.setHardLock(sw == Up);
        if (SwitchChanged() && sw == Down) { engine_.randomizeRegister(); structural = true; }

        // --- reset ---
        if (PulseIn2RisingEdge()) { engine_.resetStep(); liveDirty_ = true; }

        // --- arp pattern selection ---
        int arpPat;
        if (config_.arpPatternOverride) arpPat = config_.arpPatternOverride - 1;
        else if (Connected(Input::CV2))  arpPat = Arp::patternFromCV(CVIn2());
        else                             arpPat = ARP_UP;
        arp_.setPattern(arpPat);

        // --- clock ---
        if (PulseIn1RisingEdge()) {
            if (samplesSinceClock_ > 0 && samplesSinceClock_ < 4 * kSampleRate)
                clockPeriod_ = samplesSinceClock_;
            samplesSinceClock_ = 0;

            ++clockCounter_;
            uint8_t div = config_.chordClockDiv ? config_.chordClockDiv : 1;
            if (clockCounter_ % div == 0) {
                int tonicCV = Disconnected(Input::CV1) ? 0 : CVIn1();
                engine_.quantizeTonic(tonicCV);
                engine_.onChordClock();
                curChord_ = engine_.currentChord();
                arp_.onChordChange();
                pad_.setChord(curChord_);
                chordPulse_ = kChordPulseSamples;
                if (engine_.mutatedThisClock()) mutFlash_ = kMutationFlashSamples;
                CVOut1MIDINote((uint8_t)clampMidi(curChord_.bass));
                structural = true;
            }

            int arpNote = arp_.step(curChord_);
            CVOut2MIDINote((uint8_t)clampMidi(arpNote));
            arpPulse_ = kArpPulseSamples;
            liveDirty_ = true;
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

        // --- publish to core1 ---
        if (liveDirty_) { publishLiveSnapshot(); liveDirty_ = false; }
        if (structural) { publishFullSnapshot(); }
    }

private:
    static constexpr int kPickup = 128; // knob-zone hysteresis margin (of 4096)

    ChordEngine engine_;
    Arp         arp_;
    PadVoice    pad_;
    Config      config_;
    DecodedChord curChord_;

    int  clockCounter_ = 0;
    int  samplesSinceClock_ = 0x3fffffff;
    int  clockPeriod_ = 0;
    int  chordPulse_ = 0, arpPulse_ = 0, mutFlash_ = 0;
    bool liveDirty_ = true;
    uint32_t animCounter_ = 0;
    int  lastZone_[3] = {-1, -1, -1};

    void applyPadParams() {
        pad_.setParams(config_.padWaveform, config_.padDetuneCents,
                       config_.padCutoff, config_.padWidth, config_.padEnabled != 0);
    }

    // Knob -> zone with a little hysteresis so a knob resting on a boundary does
    // not chatter between two scales / lengths.
    int zoneHyst(int32_t raw, int zones, int idx) {
        int val = (raw * zones) >> 12;
        if (val >= zones) val = zones - 1;
        if (val < 0) val = 0;
        if (lastZone_[idx] < 0) { lastZone_[idx] = val; return val; }
        if (val != lastZone_[idx]) {
            int32_t zoneSize = 4096 / zones;
            int32_t center = lastZone_[idx] * zoneSize + zoneSize / 2;
            int32_t dist = raw - center; if (dist < 0) dist = -dist;
            if (dist > zoneSize / 2 + kPickup) lastZone_[idx] = val;
        }
        return lastZone_[idx];
    }

    bool drainCommands() {
        bool structural = false;
        shared::CmdMsg m;
        int guard = 0;
        while (shared::popCmd(m) && guard++ < 128) {
            switch (m.type) {
            case shared::C_SETSTEP: engine_.setStepValue(m.a, m.b); structural = true; break;
            case shared::C_LOCK:    engine_.setLock(m.a, m.b != 0); structural = true; break;
            case shared::C_TABLE: {
                ChordDef *slot = engine_.tableSlot(m.a, m.b, m.c);
                if (slot) {
                    slot->degree = (uint8_t)m.d; slot->type = (uint8_t)m.e;
                    slot->inversion = (int8_t)m.f; slot->octave = (int8_t)m.g;
                }
                structural = true;
                break;
            }
            case shared::C_CFG:
                applyCfg(m.a, m.b);
                structural = true;
                break;
            case shared::C_RANDOMIZE: engine_.randomizeRegister(); structural = true; break;
            case shared::C_RESET:     engine_.resetStep();         liveDirty_ = true;  break;
            case shared::C_DEFAULTS:  engine_.loadDefaultTables(); structural = true; break;
            default: break;
            }
        }
        return structural;
    }

    void applyCfg(int field, int value) {
        switch (field) {
        case shared::CF_DIV:     config_.chordClockDiv = sanitizeClockDiv((uint8_t)value); break;
        case shared::CF_ARP:     config_.arpPatternOverride = (value < 0 || value > 5) ? 0 : (uint8_t)value; break;
        case shared::CF_PADEN:   config_.padEnabled = value ? 1 : 0; break;
        case shared::CF_PADWAVE: config_.padWaveform = value ? 1 : 0; break;
        case shared::CF_PADDET:  config_.padDetuneCents = (uint8_t)(value < 0 ? 0 : (value > 50 ? 50 : value)); break;
        case shared::CF_PADCUT:  config_.padCutoff = (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value)); break;
        case shared::CF_PADWID:  config_.padWidth = (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value)); break;
        default: break;
        }
        applyPadParams();
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
        s.reg   = engine_.reg();
        s.step  = (int16_t)engine_.step();
        s.tonic = (int16_t)engine_.tonic();
        s.scale = (int16_t)engine_.scale();
        s.len   = (int16_t)engine_.length();
        s.bpmX10 = (int16_t)estimateBpmX10();
        s.mut   = engine_.mutatedThisClock() ? 1 : 0;
        shared::publishLive(s);
    }

    void publishFullSnapshot() {
        FullState fs;
        fs.version     = kPersistVersion;
        fs.config      = config_;
        fs.reg         = engine_.reg();
        fs.lockMask    = engine_.lockMask();
        fs.scale       = engine_.scale();
        fs.lengthIndex = engine_.lengthIndex();
        fs.tonic       = engine_.tonic();
        for (int s = 0; s < kNumScales; ++s) {
            for (int i = 0; i < k2BitTableLen; ++i) fs.table2[s][i] = engine_.table2_[s][i];
            for (int i = 0; i < k4BitTableLen; ++i) fs.table4[s][i] = engine_.table4_[s][i];
        }
        shared::publishFull(fs);
        // keep the live frame in sync too
        publishLiveSnapshot();
    }

    int estimateBpmX10() {
        if (clockPeriod_ <= 0) return 0;
        // Treat each incoming pulse as a beat: BPM = 60 * fs / period.
        long v = (60L * kSampleRate * 10L) / clockPeriod_;
        if (v > 99990) v = 99990;
        return (int)v;
    }
};

// ===========================================================================
// core1 - USB CDC (raw TinyUSB)
// ===========================================================================
static char     g_line[160];
static uint32_t g_lineLen = 0;
static bool     g_drop = false;
static uint32_t g_lastLiveSeq = 0;

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

static void sendStateDump() {
    if (!tud_cdc_connected()) return;
    FullState fs; uint32_t seq;
    if (!shared::readFull(fs, seq)) return;
    static char big[4608];
    int len = buildStateDump(big, sizeof(big), fs);
    if (len > 0) cdcWriteAll(big, len);
}

static void doSave() {
    FullState fs; uint32_t seq;
    if (!shared::readFull(fs, seq)) return;
    PersistState ps;
    ps.config   = fs.config;
    ps.reg      = fs.reg;
    ps.lockMask = fs.lockMask;
    for (int s = 0; s < kNumScales; ++s) {
        for (int i = 0; i < k2BitTableLen; ++i) ps.table2[s][i] = fs.table2[s][i];
        for (int i = 0; i < k4BitTableLen; ++i) ps.table4[s][i] = fs.table4[s][i];
    }
    bool ok = savePersistToFlash(ps);
    char ack[48];
    int n = snprintf(ack, sizeof(ack), "{\"c\":\"saved\",\"ok\":%s}\n", ok ? "true" : "false");
    if (n > 0) cdcWriteAll(ack, n);
}

static void dispatch(const ParsedCommand &pc) {
    using namespace shared;
    switch (pc.type) {
    case CMD_HELLO: sendStateDump(); break;
    case CMD_SAVE:  doSave(); break;
    case CMD_SETSTEP: { CmdMsg m{C_SETSTEP,(int16_t)pc.i,(int16_t)pc.v,0,0,0,0,0}; pushCmd(m); break; }
    case CMD_LOCK:    { CmdMsg m{C_LOCK,(int16_t)pc.i,(int16_t)(pc.on?1:0),0,0,0,0,0}; pushCmd(m); break; }
    case CMD_TABLE:   { CmdMsg m{C_TABLE,(int16_t)pc.scale,(int16_t)pc.mode,(int16_t)pc.slot,
                                 (int16_t)pc.deg,(int16_t)pc.ctype,(int16_t)pc.inv,(int16_t)pc.oct}; pushCmd(m); break; }
    case CMD_RANDOMIZE: { CmdMsg m{C_RANDOMIZE,0,0,0,0,0,0,0}; pushCmd(m); break; }
    case CMD_RESET:     { CmdMsg m{C_RESET,0,0,0,0,0,0,0}; pushCmd(m); break; }
    case CMD_LOADDEFAULTS: { CmdMsg m{C_DEFAULTS,0,0,0,0,0,0,0}; pushCmd(m); break; }
    case CMD_CFG: {
        if (pc.hasDiv)     { CmdMsg m{C_CFG,CF_DIV,(int16_t)pc.div,0,0,0,0,0}; pushCmd(m); }
        if (pc.hasArp)     { CmdMsg m{C_CFG,CF_ARP,(int16_t)pc.arp,0,0,0,0,0}; pushCmd(m); }
        if (pc.hasPadEn)   { CmdMsg m{C_CFG,CF_PADEN,(int16_t)pc.padEn,0,0,0,0,0}; pushCmd(m); }
        if (pc.hasPadWave) { CmdMsg m{C_CFG,CF_PADWAVE,(int16_t)pc.padWave,0,0,0,0,0}; pushCmd(m); }
        if (pc.hasPadDet)  { CmdMsg m{C_CFG,CF_PADDET,(int16_t)pc.padDet,0,0,0,0,0}; pushCmd(m); }
        if (pc.hasPadCut)  { CmdMsg m{C_CFG,CF_PADCUT,(int16_t)pc.padCut,0,0,0,0,0}; pushCmd(m); }
        if (pc.hasPadWid)  { CmdMsg m{C_CFG,CF_PADWID,(int16_t)pc.padWid,0,0,0,0,0}; pushCmd(m); }
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
    shared::LiveSnap s; uint32_t seq;
    if (!shared::readLive(s, seq)) return;
    if (seq == g_lastLiveSeq) return;
    char buf[160];
    LiveInfo li;
    li.reg = s.reg; li.step = s.step; li.tonic = s.tonic; li.scale = s.scale;
    li.len = s.len; li.bpmX10 = s.bpmX10; li.mut = s.mut != 0;
    int len = buildLiveFrame(buf, sizeof(buf), li);
    if (len <= 0) return;
    if (tud_cdc_write_available() < (uint32_t)len) return; // try again next round
    tud_cdc_write(buf, (uint32_t)len);
    tud_cdc_write_flush();
    g_lastLiveSeq = seq;
}

static void core1Main() {
    tud_init(BOARD_TUD_RHPORT);              // USB device stack lives on core1
    uint32_t lastPushMs = 0;
    const uint32_t kPushIntervalMs = 100;    // ~10 Hz live frames
    while (true) {
        tud_task();
        pollCdcInput();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((uint32_t)(now - lastPushMs) >= kPushIntervalMs) {
            sendLive();
            lastPushMs = now;
        }
    }
}

int main() {
    set_sys_clock_khz(144000, true);         // 3 x 48 MHz: low audio-input noise

    ChordLoopCard card;                      // constructed on core0 (flash/EEPROM/I2C)
    card.EnableNormalisationProbe();         // jack detection for CV1/CV2 defaults

    multicore_launch_core1(core1Main);       // USB on core1
    flash_safe_execute_core_init();          // register core0 as flash lockout victim

    card.Run();                              // 48 kHz audio DSP on core0 (never returns)
    return 0;
}
