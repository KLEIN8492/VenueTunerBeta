#include "server/Server.h"

namespace vt {

Server::Server(Dispatcher& d, AudioEngine& e, int port)
    : dispatcher(d), engine(e), ws(port, "127.0.0.1")
{
    ws.disablePerMessageDeflate();
    std::weak_ptr<bool> weak = alive;
    ws.setOnClientMessageCallback([this, weak](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& client,
                                               const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            juce::MessageManager::callAsync([this, weak] { if (weak.lock()) sendState(); });
            return;
        }
        if (msg->type != ix::WebSocketMessageType::Message || msg->binary) return;
        const std::string text = msg->str;
        ix::WebSocket* target = &client;
        juce::MessageManager::callAsync([this, weak, text, target] {
            if (!weak.lock()) return;
            const auto reply = juce::JSON::toString(dispatcher.handle(juce::JSON::parse(juce::String(text))), true);
            for (auto& c : ws.getClients()) // the client may have disconnected meanwhile
                if (c.get() == target) c->sendText(reply.toStdString());
        });
    });
    dispatcher.onStateChanged = [this] { sendState(); };
    dispatcher.onEvent = [this](const juce::var& e) { broadcastText(juce::JSON::toString(e, true)); };
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

void Server::broadcastText(const juce::String& s)
{
    const auto str = s.toStdString();
    for (auto& c : ws.getClients()) c->sendText(str);
}

void Server::broadcastBinary(const std::vector<float>& v)
{
    const std::string bytes(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
    for (auto& c : ws.getClients()) c->sendBinary(bytes);
}

void Server::timerCallback()
{
    ++tick;
    if (ws.getClients().empty()) return;
    if (tick % 3 == 0) broadcastText(juce::JSON::toString(dispatcher.statusVar(), true)); // 10 Hz

    if (tick % 2 == 0) { // TF at 15 Hz when there is new data
        const auto r = engine.analyzer().result();
        if (r.frames != lastFrames || r.delaySamples != lastDelay) {
            lastFrames = r.frames;
            const size_t n = r.freq.size();
            std::vector<float> f { 1.0f, float(n), float(r.delaySamples), float(r.frames), r.delayValid ? 1.0f : 0.0f };
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
