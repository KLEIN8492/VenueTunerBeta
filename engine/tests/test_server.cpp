#include <catch2/catch_test_macros.hpp>
#include "server/Server.h"
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <atomic>
#include <mutex>

namespace {
// What a client saw from the engine: whether it was let in, what came back, how it was closed.
struct Probe
{
    ix::WebSocket ws;
    std::mutex lock;
    std::atomic<bool> opened { false }, closed { false };
    std::atomic<int> textFrames { 0 }, closeCode { 0 };
    std::string lastText;

    explicit Probe(const std::string& url)
    {
        ws.setUrl(url);
        ws.disableAutomaticReconnection();
        ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& m) {
            if (m->type == ix::WebSocketMessageType::Open) opened = true;
            else if (m->type == ix::WebSocketMessageType::Close) { closeCode = int(m->closeInfo.code); closed = true; }
            else if (m->type == ix::WebSocketMessageType::Message && !m->binary) {
                std::lock_guard<std::mutex> l(lock);
                lastText = m->str;
                ++textFrames;
            }
        });
        ws.start();
    }
    ~Probe() { ws.stop(); }
};

// Server callbacks are marshalled to the message thread: pump it while waiting.
template <typename Pred> bool waitFor(Pred done, int ms = 3000)
{
    for (int t = 0; t < ms && !done(); t += 20) juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    return done();
}

struct ServerFixture
{
    juce::ScopedJuceInitialiser_GUI juce;
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("vt_server_test");
    vt::AudioEngine engine;
    std::unique_ptr<vt::Dispatcher> d;
    std::unique_ptr<vt::Server> server;
    static constexpr int kPort = 47893;

    explicit ServerFixture(const std::string& token)
    {
        ix::initNetSystem();
        dir.deleteRecursively();
        engine.prepareForTest(48000, 64, 2, 2);
        d = std::make_unique<vt::Dispatcher>(engine, dir);
        d->stateForTest().ensureDefaults(2, 2);
        server = std::make_unique<vt::Server>(*d, engine, kPort, token);
        REQUIRE(server->start().isEmpty());
    }
    ~ServerFixture() { server.reset(); d.reset(); dir.deleteRecursively(); }
    static std::string url(const std::string& query = {}) { return "ws://127.0.0.1:" + std::to_string(kPort) + "/" + query; }
};
}

TEST_CASE("Server: token is parsed from the request URI")
{
    CHECK(vt::Server::tokenFromUri("/?token=abc") == "abc");
    CHECK(vt::Server::tokenFromUri("/?x=1&token=abc&y=2") == "abc");
    CHECK(vt::Server::tokenFromUri("/?mytoken=abc").empty());
    CHECK(vt::Server::tokenFromUri("/").empty());
}

TEST_CASE("Server: clients without the session token are closed and never see state")
{
    ServerFixture f("s3cret-token");

    Probe none(ServerFixture::url());
    Probe wrong(ServerFixture::url("?token=guess"));
    Probe good(ServerFixture::url("?token=s3cret-token"));

    REQUIRE(waitFor([&] { return none.closed && wrong.closed && good.textFrames > 0; }));
    CHECK(none.closeCode == vt::Server::kUnauthorizedClose);
    CHECK(wrong.closeCode == vt::Server::kUnauthorizedClose);
    CHECK(none.textFrames == 0);  // no state broadcast leaked before the close
    CHECK(wrong.textFrames == 0);
    CHECK_FALSE(good.closed);

    // The authorised client can drive the engine; commands from others would be ignored.
    good.ws.sendText(R"({"id":7,"cmd":"state.get"})");
    REQUIRE(waitFor([&] { std::lock_guard<std::mutex> l(good.lock); return good.lastText.find("\"id\": 7") != std::string::npos; }));
}

TEST_CASE("Server: without a token every local client is accepted (engine started by hand)")
{
    ServerFixture f("");
    Probe c(ServerFixture::url());
    REQUIRE(waitFor([&] { return c.textFrames > 0; }));
    CHECK_FALSE(c.closed);
}
