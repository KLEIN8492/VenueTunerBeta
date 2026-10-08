#pragma once
#include "server/Dispatcher.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <memory>

namespace vt {

// WebSocket front-end. Text frames carry JSON commands/replies/events; binary frames
// carry float32 arrays:
//   TF: [1, n, delaySamples, frames, delayValid, freq[n], mag[n], phase[n], coh[n]]
//   IR: [2, n, sampleRate, delaySamples, irStart, samples[n]]
//   RTA: [3, n, splOffsetDb, calibrated, freq[n], levelDb[n]]  (dBFS; + offset = dB SPL)
// Network callbacks are marshalled to the message thread before touching state.
class Server : private juce::Timer
{
public:
    Server(Dispatcher&, AudioEngine&, int port);
    ~Server() override;
    juce::String start(); // empty on success

private:
    void timerCallback() override;
    void broadcastText(const juce::String&);
    void broadcastBinary(const std::vector<float>&);
    void sendState();

    Dispatcher& dispatcher;
    AudioEngine& engine;
    ix::WebSocketServer ws;
    int tick = 0, lastFrames = -1;
    double lastDelay = -1;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

} // namespace vt
