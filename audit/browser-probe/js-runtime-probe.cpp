#include <QCoreApplication>
#include <QJSEngine>
#include <QJSValue>
#include <QDebug>
#include <thread>
#include <chrono>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QJSEngine engine;
    const QStringList scripts{
        "typeof Promise",
        "(async () => { return await Promise.resolve(7); })()",
        "globalThis.savedValue=23; savedValue",
        "globalThis.savedValue + 1",
        "typeof BigInt",
        "typeof fetch",
        "typeof Buffer",
        "typeof require"
    };
    for (const auto& script : scripts) {
        const auto value = engine.evaluate(script);
        qInfo().noquote() << script << "=>" << value.toString() << "error=" << value.isError();
    }
    std::thread interrupt([&engine] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        engine.setInterrupted(true);
    });
    const auto result = engine.evaluate("while(true) {}");
    interrupt.join();
    qInfo().noquote() << "bounded interruption =>" << result.toString() << "interrupted=" << engine.isInterrupted();
}
