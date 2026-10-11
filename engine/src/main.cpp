// VenueTuner engine: headless audio processor + analyzer, controlled over WebSocket.
//   VenueTunerEngine [--port 47800] [--data-dir <dir>] [--quit-on-stdin-eof]
// --quit-on-stdin-eof: stop cleanly (autosave) when the parent closes our stdin. Windows has no
// SIGTERM for child processes, so this is how the desktop shell asks the engine to quit there.
#include "core/AudioEngine.h"
#include "server/Dispatcher.h"
#include "server/Server.h"
#include <ixwebsocket/IXNetSystem.h>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> quitRequested { false };
void onSignal(int) { quitRequested.store(true); }

}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit; // message thread (no windows are created)

    int port = 47800;
    // macOS ~/Library/Application Support, Windows %APPDATA%, Linux ~/.config
#if JUCE_MAC
    const char* dataSub = "Application Support/VenueTuner";
#else
    const char* dataSub = "VenueTuner";
#endif
    juce::File dataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile(dataSub);
    bool quitOnStdinEof = false;
    for (int i = 1; i < argc; ++i) {
        const juce::String a(argv[i]);
        if (a == "--port" && i + 1 < argc) port = juce::String(argv[++i]).getIntValue();
        else if (a == "--data-dir" && i + 1 < argc) dataDir = juce::File(juce::String(argv[++i]));
        else if (a == "--quit-on-stdin-eof") quitOnStdinEof = true;
    }
    dataDir.createDirectory();

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    if (quitOnStdinEof)
        std::thread([] { char c; while (std::cin.get(c)) {} quitRequested.store(true); }).detach();

    ix::initNetSystem(); // Windows: WSAStartup before any socket (no-op elsewhere)
    struct NetSystem { ~NetSystem() { ix::uninitNetSystem(); } } netSystem;

    vt::AudioEngine engine;
    vt::Dispatcher dispatcher(engine, dataDir);
    vt::Server server(dispatcher, engine, port);
    if (auto err = server.start(); err.isNotEmpty()) {
        std::cerr << "VenueTuner engine: cannot listen on port " << port << ": " << err << std::endl;
        return 2;
    }
    dispatcher.start();
    std::cout << "VENUETUNER_READY port=" << port << std::endl;

    // runDispatchLoop() returns at once in a non-bundled macOS process, so pump the
    // message loop in slices until a signal asks us to stop.
    auto* mm = juce::MessageManager::getInstance();
    while (!quitRequested.load())
        mm->runDispatchLoopUntil(100);
    dispatcher.shutdown();
    std::cout << "VenueTuner engine: stopped" << std::endl;
    return 0;
}
