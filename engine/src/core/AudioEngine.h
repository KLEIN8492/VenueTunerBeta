#pragma once
#include "core/Params.h"
#include "core/SystemState.h"
#include "dsp/OutputChain.h"
#include "dsp/PinkNoise.h"
#include "dsp/SweepGen.h"
#include "measure/Analyzer.h"
#include "rt/TripleBuffer.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>

namespace vt {

// Owns the audio device and runs the real-time path:
//   inputs → routing matrix (+ generator) → OutputChain per output → outputs
// and feeds the analyzer with (reference, mic). Outputs are muted after start-up and
// after any device fault until unmuteAfterFault() is called.
class AudioEngine : private juce::AudioIODeviceCallback, private juce::ChangeListener, private juce::Timer
{
public:
    struct Status
    {
        juce::String inputDevice, outputDevice;
        double sampleRate = 0;
        int bufferSize = 0, numIn = 0, numOut = 0;
        double cpu = 0;
        int xruns = 0;
        int64_t callbacks = 0; // audio callbacks since start (health check)
        int driverLatencySamples = 0; // input + output as reported by the driver
        bool running = false, faultMuted = true, muteAll = false;
        juce::String lastError;
        std::array<float, kMaxCh> inPeak {}, outPeak {}, limGrDb {};
    };

    AudioEngine();
    ~AudioEngine() override;

    // Message thread. Returns an empty string on success, else the error.
    juce::String openDevice(const DeviceConfig&);
    void closeDevice();
    juce::var listDevices();
    juce::var channelNames();     // {inputs:[..], outputs:[..]} driver channel names
    juce::var latencyBreakdown(); // per-direction driver latency components

    void setParams(const Params& p) { params.write(p); }
    // Any thread: the convolution swaps the IR in on its background thread and crossfades.
    void loadFir(int slot, juce::AudioBuffer<float>&& ir, double irSampleRate);
    void setMuteAll(bool on) { muteAll.store(on); }
    void unmuteAfterFault() { faultMuted.store(false); }
    Status status(); // also resets peak holds
    Analyzer& analyzer() { return analyzerImpl; }

    // Test hooks: run the callback without a device.
    void prepareForTest(double sr, int block, int numIn, int numOut);
    void simulateDeviceFault() { faultMuted.store(true); }
    void processBlock(const float* const* in, int numIn, float* const* out, int numOut, int n);

private:
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int n, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String&) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override; // reconnect watchdog
    void prepare(double sr, int block);
    juce::String ensureInitialised();

    struct AnalysisThread : juce::Thread
    {
        explicit AnalysisThread(Analyzer& a) : juce::Thread("vt-analysis"), an(a) {}
        void run() override { while (!threadShouldExit()) { an.process(); wait(10); } }
        Analyzer& an;
    };

    juce::AudioDeviceManager dm;
    DeviceConfig wanted;
    bool deviceWanted = false, initialised = false;
    juce::String lastError;

    TripleBuffer<Params> params;
    std::array<OutputChain, kMaxCh> chains;
    juce::dsp::ConvolutionMessageQueue convQueue;
    std::array<std::unique_ptr<juce::dsp::Convolution>, kMaxCh> convs;
    PinkNoise pink;
    SweepGen sweep;
    int64_t sweepPos = 0;
    uint32_t lastSweepId = 0;
    double currentSr = 48000.0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> genGain;
    std::vector<float> genBuf, mixBuf, zeroBuf;
    std::atomic<bool> muteAll { false }, faultMuted { true };
    std::atomic<int> xruns { 0 };
    std::atomic<int64_t> callbackCount { 0 };
    std::array<std::atomic<float>, kMaxCh> inPeak {};
    Analyzer analyzerImpl;
    AnalysisThread analysisThread { analyzerImpl };
};

} // namespace vt
