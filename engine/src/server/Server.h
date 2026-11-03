#pragma once
#include "server/Dispatcher.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace vt {

// WebSocket front-end. Text frames carry JSON commands/replies/events; binary frames
// carry float32 arrays:
//   TF: [1, n, delaySamples, frames, delayValid, delayLocked, autoDelaySamples, freq[n], mag[n], phase[n], coh[n]]
//   IR: [2, n, sampleRate, delaySamples, irStart, samples[n]]
//   RTA: [3, n, splOffsetDb, calibrated, freq[n], levelDb[n]]  (dBFS; + offset = dB SPL)
// Network callbacks are marshalled to the message thread before touching state.
//
// Authentication (session token): this port controls a PA (mute, generator level, routing), so
// a local process must not be able to drive it just by connecting. The desktop shell creates a
// random token per launch and hands it to the engine on stdin (--token-stdin: not in argv, which
// `ps` shows to everyone, nor in the environment). Clients connect to ws://127.0.0.1:<port>/?token=<t>;
// the token is checked in the WebSocket opening handshake (Open). A client without the right token
// is logged, closed with code 4001 and never receives state, events or spectra. With an empty
// token (engine started by hand, tests) every local client is accepted and a warning is printed.
class Server : private juce::Timer
{
public:
    Server(Dispatcher&, AudioEngine&, int port, std::string token = {});
    static constexpr uint16_t kUnauthorizedClose = 4001;
    // Token from a request URI ("/?token=abc&x=1" → "abc"); empty if absent.
    static std::string tokenFromUri(const std::string& uri);
    ~Server() override;
    juce::String start(); // empty on success

private:
    void timerCallback() override;
    void broadcastText(const juce::String&);
    void broadcastBinary(const std::vector<float>&);
    void sendState();
    bool authorised(ix::WebSocket*) const;

    const std::string token;
    mutable std::mutex authLock; // network threads add/remove, the message thread reads
    std::set<ix::WebSocket*> authed;

    Dispatcher& dispatcher;
    AudioEngine& engine;
    ix::WebSocketServer ws;
    int tick = 0, lastFrames = -1;
    double lastDelay = -1;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

} // namespace vt
