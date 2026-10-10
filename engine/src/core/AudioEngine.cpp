#include "core/AudioEngine.h"
#include "core/LatencyInfo.h"

namespace vt {

namespace {
constexpr float kGenFloor = 1e-5f; // -100 dB: start/end point of the generator fade
}

AudioEngine::AudioEngine()
{
    for (size_t o = 0; o < convs.size(); ++o) {
        convs[o] = std::make_unique<juce::dsp::Convolution>(juce::dsp::Convolution::NonUniform { 256 }, convQueue);
        chains[o].setConvolution(convs[o].get());
    }
}

void AudioEngine::loadFir(int output, juce::AudioBuffer<float>&& ir, double irSampleRate)
{
    if (output < 0 || output >= kMaxCh) return;
    convs[size_t(output)]->loadImpulseResponse(std::move(ir), irSampleRate, juce::dsp::Convolution::Stereo::no,
                                               juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
}

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
    // Empty names (first launch, fresh install) mean "the system's default devices", not none.
    setup.inputDeviceName = cfg.inputDevice;
    setup.outputDeviceName = cfg.outputDevice;
    // Only scan when a name is missing: a scan queries every CoreAudio driver, and one
    // misbehaving driver (e.g. a stuck virtual soundcard) can make it hang.
    auto* type = setup.outputDeviceName.isEmpty() || setup.inputDeviceName.isEmpty() ? dm.getCurrentDeviceTypeObject() : nullptr;
    if (type != nullptr) {
        type->scanForDevices();
        auto defaultName = [type](bool input) {
            const auto names = type->getDeviceNames(input);
            const int i = type->getDefaultDeviceIndex(input);
            return names.isEmpty() ? juce::String() : names[juce::jlimit(0, names.size() - 1, i)];
        };
        if (setup.outputDeviceName.isEmpty()) setup.outputDeviceName = defaultName(false);
        if (setup.inputDeviceName.isEmpty()) setup.inputDeviceName = defaultName(true);
    }
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

juce::var AudioEngine::channelNames()
{
    auto v = juce::var(new juce::DynamicObject());
    juce::Array<juce::var> ins, outs;
    if (auto* dev = dm.getCurrentAudioDevice()) {
        const auto inNames = dev->getInputChannelNames(), outNames = dev->getOutputChannelNames();
        const int ni = dev->getActiveInputChannels().countNumberOfSetBits(), no = dev->getActiveOutputChannels().countNumberOfSetBits();
        for (int i = 0; i < ni; ++i) ins.add(i < inNames.size() && inNames[i].isNotEmpty() ? inNames[i] : "In " + juce::String(i + 1));
        for (int i = 0; i < no; ++i) outs.add(i < outNames.size() && outNames[i].isNotEmpty() ? outNames[i] : "Out " + juce::String(i + 1));
    }
    v.getDynamicObject()->setProperty("inputs", ins);
    v.getDynamicObject()->setProperty("outputs", outs);
    return v;
}

juce::var AudioEngine::latencyBreakdown()
{
    const auto setup = dm.getAudioDeviceSetup();
    auto* dev = dm.getCurrentAudioDevice();
    return vt::latencyBreakdown(setup.inputDeviceName, setup.outputDeviceName, dev ? dev->getCurrentBufferSizeSamples() : 0);
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
    currentSr = sr;
    const int maxBlock = std::max(block, 4096);
    for (auto& c : chains) c.prepare(sr, maxBlock);
    genBuf.assign(size_t(maxBlock), 0.0f);
    mixBuf.assign(size_t(maxBlock), 0.0f);
    zeroBuf.assign(size_t(maxBlock), 0.0f);
    genGain.reset(sr, 0.5); // 500 ms fade in/out
    genGain.setCurrentAndTargetValue(kGenFloor);
    analyzerImpl.requestReset(sr);
    meters.setSampleRate(sr); // the Leq only restarts if the rate really changed
    for (auto& a : extra)
        if (auto* an = a.load()) an->requestReset(sr);
}

void AudioEngine::ensureExtraAnalyzers(int count)
{
    count = std::clamp(count, 0, kMaxMics - 1);
    for (int i = int(extraStorage.size()); i < count; ++i) {
        extraStorage.push_back(std::make_unique<Analyzer>());
        extraStorage.back()->requestReset(currentSr);
        extra[size_t(i)].store(extraStorage.back().get(), std::memory_order_release);
    }
}

void AudioEngine::audioDeviceStopped() { faultMuted.store(true); }
void AudioEngine::audioDeviceError(const juce::String& e)
{
    faultMuted.store(true);
    // May arrive on a driver thread: hand the message to the message thread (no shared String).
    juce::MessageManager::callAsync([this, e] { lastError = e; });
}

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
    // Flush denormals to zero: decaying IIR states on silence otherwise get 10–100× slower
    // on x86 (Intel Macs, mini PCs) exactly when the room goes quiet.
    const juce::ScopedNoDenormals noDenormals;
    if (n > int(genBuf.size())) { // should never happen; stay silent rather than allocate
        for (int o = 0; o < numOut; ++o) if (out[o]) juce::FloatVectorOperations::clear(out[o], n);
        xruns.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const Params& p = params.read();
    const bool silence = muteAll.load(std::memory_order_relaxed) || faultMuted.load(std::memory_order_relaxed);

    const float level = std::pow(10.0f, std::min(p.gen.levelDb, kGeneratorMaxDb) / 20.0f);
    if (p.gen.on && p.gen.mode == GeneratorParams::Sweep) {
        // Log sweep, played once per sweepId. Level is RMS, so peak = level · √2.
        if (p.gen.sweepId != lastSweepId) {
            lastSweepId = p.gen.sweepId;
            sweepPos = 0;
            sweep.sr = currentSr;
            sweep.length = p.gen.sweepLength;
        }
        for (int i = 0; i < n; ++i) genBuf[size_t(i)] = sweep.sample(sweepPos++) * level * 1.41421356f;
        genGain.setCurrentAndTargetValue(kGenFloor); // pink noise fades in again afterwards
    } else {
        // Pink noise (unit RMS) with a smooth fade in/out.
        genGain.setTargetValue(p.gen.on ? level : kGenFloor);
        for (int i = 0; i < n; ++i) {
            const float g = genGain.getNextValue();
            genBuf[size_t(i)] = g <= kGenFloor * 1.0001f ? 0.0f : pink.next() * g;
        }
    }

    for (int c = 0; c < numIn && c < kMaxCh; ++c) {
        if (in[c] == nullptr) continue;
        const auto r = juce::FloatVectorOperations::findMinAndMax(in[c], n);
        const float pk = std::max(-r.getStart(), r.getEnd());
        if (pk > inPeak[size_t(c)].load(std::memory_order_relaxed)) inPeak[size_t(c)].store(pk, std::memory_order_relaxed);
    }

    // Physical outputs nothing is patched to stay silent.
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clear(out[c], n);

    // Each logical output: sum its program inputs (+ generator) → chain → its physical channel.
    // Unassigned outputs are still processed (meters, filter state) but go nowhere.
    for (int o = 0; o < p.numOut && o < kMaxCh; ++o) {
        const auto& op = p.out[o];
        float* buf = mixBuf.data();
        juce::FloatVectorOperations::clear(buf, n);
        for (int c = 0; c < numIn && c < kMaxCh; ++c)
            if (in[c] != nullptr && op.mix[c] != 0.0f && (p.consoleInputs >> c & 1u))
                juce::FloatVectorOperations::addWithMultiply(buf, in[c], op.mix[c], n);
        if (p.gen.targets >> o & 1u)
            juce::FloatVectorOperations::add(buf, genBuf.data(), n);
        const int slot = std::clamp(op.slot, 0, kMaxCh - 1);
        chains[size_t(slot)].process(op, buf, n, silence);
        if (op.channel >= 0 && op.channel < numOut && out[op.channel] != nullptr)
            juce::FloatVectorOperations::copy(out[op.channel], buf, n);
    }

    // Sound level / RTA: always the user's measurement mic, whatever a job does with the analyzer.
    const auto& m = p.measure;
    if (m.splInput >= 0 && m.splInput < numIn && in[m.splInput] != nullptr)
        meters.push(in[m.splInput], n, m.splInput);

    // Measurement: reference = generator (internal loopback) or a physical input.
    if (m.micInput >= 0 && m.micInput < numIn && in[m.micInput] != nullptr) {
        const float* ref = genBuf.data();
        if (m.ref != RefSource::Generator)
            ref = (m.refInput >= 0 && m.refInput < numIn && in[m.refInput] != nullptr) ? in[m.refInput] : zeroBuf.data();
        analyzerImpl.pushPair(ref, in[m.micInput], n);
        // Extra mics: same reference, measured at the same time.
        for (int k = 0; k < m.numExtra && k < kMaxMics - 1; ++k) {
            const int ch = m.extraMicInputs[k];
            auto* an = extra[size_t(k)].load(std::memory_order_acquire);
            if (an == nullptr || ch < 0 || ch >= numIn || in[ch] == nullptr) continue;
            an->pushPair(ref, in[ch], n);
        }
    }
}

} // namespace vt
