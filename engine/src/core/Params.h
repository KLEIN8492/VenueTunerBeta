#pragma once
#include "dsp/Biquad.h"
#include <cstdint>

namespace vt {

constexpr int kMaxCh = 64;
// IIR slots per output: 16 PEQ + 4 HP + 4 LP + 4 all-pass (fixed positions so ramps line up).
constexpr int kIirPeq = 0, kIirHp = 16, kIirLp = 20, kIirAp = 24, kMaxIir = 28;

// Everything the audio thread needs, as plain data (copied through a TripleBuffer).
struct OutputParams
{
    int channel = -1;  // physical output this processing channel writes to
    int slot = 0;      // engine processing slot (chain + FIR); stable across patch edits
    uint32_t slotGen = 0; // changes when the slot is handed to a different output → reset
    float gainDb = 0.0f;
    bool polarityInvert = false;
    float delayMs = 0.0f;
    bool mute = false;
    bool limiterOn = true;
    float limThreshDb = -1.0f;
    float limAttackMs = 0.0f;
    float limReleaseMs = 100.0f;
    float mix[kMaxCh] {}; // linear gain from each physical input (resolved from program sources)
    Biquad iir[kMaxIir];  // identity = unused slot
    bool firOn = false;     // FIR in the signal path
    bool firLoaded = false; // an IR is loaded: convolution keeps running so on/off can crossfade
};

enum class RefSource : uint8_t { Generator = 0, PhysicalInput = 1, ConsoleInput = 2 };

struct GeneratorParams
{
    enum Mode : uint8_t { Pink = 0, Sweep = 1 };
    bool on = false;
    Mode mode = Pink;
    int64_t sweepLength = 0; // samples
    uint32_t sweepId = 0;    // a new id (re)starts the sweep once
    float levelDb = -40.0f;   // dBFS RMS
    uint64_t targets = 0;     // bitmask of outputs
};

struct MeasureParams
{
    int micInput = -1;          // physical input index, -1 = none
    RefSource ref = RefSource::Generator;
    int refInput = -1;          // for PhysicalInput / ConsoleInput
};

struct Params
{
    int numOut = 0;
    uint64_t consoleInputs = 0; // only these inputs may be routed to outputs
    OutputParams out[kMaxCh];
    GeneratorParams gen;
    MeasureParams measure;
};

} // namespace vt
