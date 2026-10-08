#include "core/AudioEngine.h"

namespace vt {

namespace {
constexpr float kGenFloor = 1e-5f; // -100 dB: start/end point of the generator fade
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    stopTimer();
    dm.removeChangeListener(this);
    dm.removeAudioCallback(this);
    dm.closeAudioDevice();
    analysisThread.stopThread(1000);
}

juce::String AudioEngine::ensureInitialised()
{
    if (initialised) return {};
    if (auto err = dm.initialise(kMaxCh, kMaxCh, nullptr, false); err.isNotEmpty()) return err;
    dm.addChangeListener(this);
    dm.addAudioCallback(this);
    initialised = true;
    return {};
}

juce::String AudioEngine::openDevice(const DeviceConfig& cfg)
{
    wanted = cfg;
    deviceWanted = true;
    if (auto err = ensureInitialised(); err.isNotEmpty()) return lastError = err;
    auto setup = dm.getAudioDeviceSetup();
    setup.inputDeviceName = cfg.inputDevice;
    setup.outputDeviceName = cfg.outputDevice;
    setup.sampleRate = cfg.sampleRate;
    setup.bufferSize = cfg.bufferSize;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.outputChannels.clear();
    setup.inputChannels.setRange(0, kMaxCh, true);
    setup.outputChannels.setRange(0, kMaxCh, true);
    faultMuted.store(true); // any device change starts muted
    lastError = dm.setAudioDeviceSetup(setup, true);
    startTimer(1000);
    return lastError;
}

void AudioEngine::closeDevice()
{
    deviceWanted = false;
    faultMuted.store(true);
    dm.closeAudioDevice();
}

juce::var AudioEngine::listDevices()
{
    ensureInitialised();
    auto root = juce::var(new juce::DynamicObject());
    juce::Array<juce::var> ins, outs, rates, buffers;
    if (auto* type = dm.getCurrentDeviceTypeObject()) {
        type->scanForDevices();
        for (auto& n : type->getDeviceNames(true)) ins.add(n);
        for (auto& n : type->getDeviceNames(false)) outs.add(n);
    }
    if (auto* dev = dm.getCurrentAudioDevice()) {
        for (auto r : dev->getAvailableSampleRates()) rates.add(r);
        for (auto b : dev->getAvailableBufferSizes()) buffers.add(b);
    }
    root.getDynamicObject()->setProperty("inputs", ins);
    root.getDynamicObject()->setProperty("outputs", outs);
    root.getDynamicObject()->setProperty("sampleRates", rates);
    root.getDynamicObject()->setProperty("bufferSizes", buffers);
    return root;
}

AudioEngine::Status AudioEngine::status()
{
    Status s;
    if (auto* dev = dm.getCurrentAudioDevice()) {
        const auto setup = dm.getAudioDeviceSetup();
        s.inputDevice = setup.inputDeviceName;
        s.outputDevice = setup.outputDeviceName;
        s.sampleRate = dev->getCurrentSampleRate();
        s.bufferSize = dev->getCurrentBufferSizeSamples();
        s.numIn = dev->getActiveInputChannels().countNumberOfSetBits();
        s.numOut = dev->getActiveOutputChannels().countNumberOfSetBits();
        s.driverLatencySamples = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();
        s.running = dev->isPlaying();
        s.xruns = dev->getXRunCount() + xruns.load();
    }
    s.cpu = dm.getCpuUsage();
    s.callbacks = callbackCount.load(std::memory_order_relaxed);
    s.faultMuted = faultMuted.load();
    s.muteAll = muteAll.load();
    s.lastError = lastError;
    for (int c = 0; c < kMaxCh; ++c) {
        s.inPeak[size_t(c)] = inPeak[size_t(c)].exchange(0.0f, std::memory_order_relaxed);
        s.outPeak[size_t(c)] = chains[size_t(c)].takePeak();
        s.limGrDb[size_t(c)] = chains[size_t(c)].takeGainReductionDb();
    }
    return s;
}

// ------------------------------------------------------------------ device events
void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* dev)
{
    prepare(dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples());
    // The analysis thread is the analyzer's only consumer; started with the first device
    // (tests drive analyzer().process() themselves and never start it).
    if (!analysisThread.isThreadRunning()) analysisThread.startThread(juce::Thread::Priority::normal);
}

void AudioEngine::prepareForTest(double sr, int block, int, int) { prepare(sr, block); }

void AudioEngine::prepare(double sr, int block)
{
    const int maxBlock = std::max(block, 4096);
    for (auto& c : chains) c.prepare(sr, maxBlock);
    genBuf.assign(size_t(maxBlock), 0.0f);
    mixBuf.assign(size_t(maxBlock), 0.0f);
    zeroBuf.assign(size_t(maxBlock), 0.0f);
    genGain.reset(sr, 0.5); // 500 ms fade in/out
    genGain.setCurrentAndTargetValue(kGenFloor);
    analyzerImpl.requestReset(sr);
}

void AudioEngine::audioDeviceStopped() { faultMuted.store(true); }
void AudioEngine::audioDeviceError(const juce::String& e) { faultMuted.store(true); lastError = e; }

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster*)
{
    auto* dev = dm.getCurrentAudioDevice();
    if (dev == nullptr || !dev->isPlaying()) faultMuted.store(true);
}

void AudioEngine::timerCallback()
{
    // Reconnect watchdog: if the wanted device vanished, retry every second.
    auto* dev = dm.getCurrentAudioDevice();
    if (deviceWanted && (dev == nullptr || !dev->isPlaying())) {
        faultMuted.store(true);
        auto setup = dm.getAudioDeviceSetup();
        setup.inputDeviceName = wanted.inputDevice;
        setup.outputDeviceName = wanted.outputDevice;
        setup.sampleRate = wanted.sampleRate;
        setup.bufferSize = wanted.bufferSize;
        lastError = dm.setAudioDeviceSetup(setup, true);
    }
}

// ------------------------------------------------------------------ real-time path
void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out,
                                                   int numOut, int n, const juce::AudioIODeviceCallbackContext&)
{
    callbackCount.fetch_add(1, std::memory_order_relaxed);
    processBlock(in, numIn, out, numOut, n);
}

void AudioEngine::processBlock(const float* const* in, int numIn, float* const* out, int numOut, int n)
{
    if (n > int(genBuf.size())) { // should never happen; stay silent rather than allocate
        for (int o = 0; o < numOut; ++o) if (out[o]) juce::FloatVectorOperations::clear(out[o], n);
        xruns.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const Params& p = params.read();
    const bool silence = muteAll.load(std::memory_order_relaxed) || faultMuted.load(std::memory_order_relaxed);

    // Generator (pink noise, unit RMS) with a smooth fade in/out.
    const float genTarget = p.gen.on ? std::pow(10.0f, std::min(p.gen.levelDb, kGeneratorMaxDb) / 20.0f) : kGenFloor;
    genGain.setTargetValue(genTarget);
    for (int i = 0; i < n; ++i) {
        const float g = genGain.getNextValue();
        genBuf[size_t(i)] = g <= kGenFloor * 1.0001f ? 0.0f : pink.next() * g;
    }

    for (int c = 0; c < numIn && c < kMaxCh; ++c) {
        if (in[c] == nullptr) continue;
        const auto r = juce::FloatVectorOperations::findMinAndMax(in[c], n);
        const float pk = std::max(-r.getStart(), r.getEnd());
        if (pk > inPeak[size_t(c)].load(std::memory_order_relaxed)) inPeak[size_t(c)].store(pk, std::memory_order_relaxed);
    }

    for (int o = 0; o < numOut; ++o) {
        if (out[o] == nullptr) continue;
        if (o >= p.numOut || o >= kMaxCh) { juce::FloatVectorOperations::clear(out[o], n); continue; }
        const auto& op = p.out[o];
        float* buf = mixBuf.data();
        juce::FloatVectorOperations::clear(buf, n);
        for (int c = 0; c < numIn && c < kMaxCh; ++c)
            if (in[c] != nullptr && op.mix[c] != 0.0f && (p.consoleInputs >> c & 1u))
                juce::FloatVectorOperations::addWithMultiply(buf, in[c], op.mix[c], n);
        if (p.gen.targets >> o & 1u)
            juce::FloatVectorOperations::add(buf, genBuf.data(), n);
        chains[size_t(o)].process(op, buf, n, silence);
        juce::FloatVectorOperations::copy(out[o], buf, n);
    }

    // Measurement: reference = generator (internal loopback) or a physical input.
    const auto& m = p.measure;
    if (m.micInput >= 0 && m.micInput < numIn && in[m.micInput] != nullptr) {
        const float* ref = genBuf.data();
        if (m.ref != RefSource::Generator)
            ref = (m.refInput >= 0 && m.refInput < numIn && in[m.refInput] != nullptr) ? in[m.refInput] : zeroBuf.data();
        analyzerImpl.pushRef(ref, n);
        analyzerImpl.pushMeas(in[m.micInput], n);
    }
}

} // namespace vt
