#include "server/Server.h"
#include <exception>
#include <iostream>

namespace vt {

namespace {
// Constant-time comparison: the time taken doesn't tell an attacker how many characters matched.
bool sameToken(const std::string& a, const std::string& b)
{
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    for (size_t i = 0; i < a.size(); ++i) diff |= uint8_t(a[i] ^ (i < b.size() ? b[i] : 0));
    return diff == 0;
}
}

std::string Server::tokenFromUri(const std::string& uri)
{
    const auto q = uri.find('?');
    if (q == std::string::npos) return {};
    size_t at = q + 1;
    while (at < uri.size()) {
        const auto end = std::min(uri.find('&', at), uri.size());
        if (uri.compare(at, 6, "token=") == 0) return uri.substr(at + 6, end - at - 6);
        at = end + 1;
    }
    return {};
}

Server::Server(Dispatcher& d, AudioEngine& e, int port, std::string sessionToken)
    : dispatcher(d), engine(e), ws(port, "127.0.0.1"), token(std::move(sessionToken))
{
    ws.disablePerMessageDeflate();
    if (token.empty())
        std::cerr << "VenueTuner engine: no session token, any local process can control the outputs" << std::endl;
    std::weak_ptr<bool> weak = alive;
    ws.setOnClientMessageCallback([this, weak](std::shared_ptr<ix::ConnectionState> conn, ix::WebSocket& client,
                                               const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            if (!token.empty() && !sameToken(tokenFromUri(msg->openInfo.uri), token)) {
                std::cerr << "VenueTuner engine: rejected connection from " << (conn ? conn->getRemoteIp() : std::string("?"))
                          << " (missing or wrong session token)" << std::endl;
                client.close(kUnauthorizedClose, "unauthorized");
                return;
            }
            { std::lock_guard<std::mutex> l(authLock); authed.insert(&client); }
            juce::MessageManager::callAsync([this, weak] { if (weak.lock()) sendState(); });
            return;
        }
        if (msg->type == ix::WebSocketMessageType::Close) {
            std::lock_guard<std::mutex> l(authLock);
            authed.erase(&client);
            return;
        }
        if (msg->type != ix::WebSocketMessageType::Message || msg->binary) return;
        if (!authorised(&client)) return; // commands only from clients that passed the handshake
        const std::string text = msg->str;
        ix::WebSocket* target = &client;
        juce::MessageManager::callAsync([this, weak, text, target] {
            if (!weak.lock()) return;
            juce::String reply;
            try {
                const auto parsed = juce::JSON::parse(juce::String(text));
                reply = juce::JSON::toString(dispatcher.handle(parsed), true);
            } catch (const std::exception& e) {
                juce::var err(new juce::DynamicObject());
                err.getDynamicObject()->setProperty("ok", false);
                err.getDynamicObject()->setProperty("error", juce::String(e.what()));
                reply = juce::JSON::toString(err, true);
            }
            for (auto& c : ws.getClients()) // the client may have disconnected meanwhile
                if (c.get() == target && authorised(c.get())) c->sendText(reply.toStdString());
        });
    });
    dispatcher.onStateChanged = [this] { sendState(); };
    dispatcher.onEvent = [this](const juce::var& event) { broadcastText(juce::JSON::toString(event, true)); };
}

Server::~Server()
{
    stopTimer();
    *alive = false;
    dispatcher.onStateChanged = nullptr;
    dispatcher.onEvent = nullptr;
    ws.stop();
}

juce::String Server::start()
{
    auto res = ws.listen();
    if (!res.first) return res.second;
    ws.start();
    startTimerHz(30);
    return {};
}

void Server::sendState()
{
    auto e = juce::var(new juce::DynamicObject());
    e.getDynamicObject()->setProperty("event", "state");
    e.getDynamicObject()->setProperty("state", toVar(dispatcher.getState()));
    broadcastText(juce::JSON::toString(e, true));
}

bool Server::authorised(ix::WebSocket* c) const
{
    std::lock_guard<std::mutex> l(authLock);
    return authed.count(c) > 0;
}

void Server::broadcastText(const juce::String& s)
{
    const auto str = s.toStdString();
    for (auto& c : ws.getClients()) if (authorised(c.get())) c->sendText(str);
}

void Server::broadcastBinary(const std::vector<float>& v)
{
    const std::string bytes(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
    for (auto& c : ws.getClients()) if (authorised(c.get())) c->sendBinary(bytes);
}

void Server::timerCallback()
{
    ++tick;
    if (ws.getClients().empty()) return;
    if (tick % 3 == 0) { // 10 Hz
        broadcastText(juce::JSON::toString(dispatcher.statusVar(), true));
        auto rta = engine.rta().result();
        if (!rta.freq.empty()) {
            const auto [off, cal] = dispatcher.splOffset();
            // Same mic correction as the transfer function (the file is 0 dB at 1 kHz, so the
            // SPL calibration is unaffected).
            for (size_t i = 0; i < rta.freq.size(); ++i) rta.levelDb[i] -= dispatcher.micCalibration().offsetDb(rta.freq[i]);
            std::vector<float> f { 3.0f, float(rta.freq.size()), float(off), cal ? 1.0f : 0.0f };
            f.insert(f.end(), rta.freq.begin(), rta.freq.end());
            f.insert(f.end(), rta.levelDb.begin(), rta.levelDb.end());
            broadcastBinary(f);
        }
    }

    if (tick % 2 == 0) { // TF at 15 Hz when there is new data
        const auto r = engine.analyzer().result();
        if (r.frames != lastFrames || r.delaySamples != lastDelay) {
            lastFrames = r.frames;
            const size_t n = r.freq.size();
            std::vector<float> f { 1.0f, float(n), float(r.delaySamples), float(r.frames), r.delayValid ? 1.0f : 0.0f,
                                   r.delayLocked ? 1.0f : 0.0f, float(r.autoDelaySamples) };
            f.insert(f.end(), r.freq.begin(), r.freq.end());
            f.insert(f.end(), r.magDb.begin(), r.magDb.end());
            f.insert(f.end(), r.phaseDeg.begin(), r.phaseDeg.end());
            f.insert(f.end(), r.coherence.begin(), r.coherence.end());
            broadcastBinary(f);
            if (r.delaySamples != lastDelay || tick % 15 == 0) { // IR at ~2 Hz
                lastDelay = r.delaySamples;
                std::vector<float> ir { 2.0f, float(r.ir.size()), r.sampleRate, float(r.delaySamples), float(r.irStart) };
                ir.insert(ir.end(), r.ir.begin(), r.ir.end());
                broadcastBinary(ir);
            }
        }
    }
}

} // namespace vt
