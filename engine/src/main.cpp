// VenueTuner engine: headless audio processor + analyzer, controlled over WebSocket.
//   VenueTunerEngine [--port 47800] [--data-dir <dir>]
#include "core/AudioEngine.h"
#include "server/Dispatcher.h"
#include "server/Server.h"
#include <csignal>
#include <iostream>

namespace {
std::atomic<bool> quitRequested { false };
void onSignal(int) { quitRequested.store(true); }

}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit; // message thread (no windows are created)

    int port = 47800;
    juce::File dataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                             .getChildFile("Application Support/VenueTuner");
    for (int i = 1; i + 1 < argc; ++i) {
        const juce::String a(argv[i]);
        if (a == "--port") port = juce::String(argv[++i]).getIntValue();
        else if (a == "--data-dir") dataDir = juce::File(juce::String(argv[++i]));
    }
    dataDir.createDirectory();

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

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
