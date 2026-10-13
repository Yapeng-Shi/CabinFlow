#include "voice_client.hpp"
#include "wake_microphone.hpp"

#include <iostream>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <csignal>

namespace {
volatile std::sig_atomic_t stopping = 0;
void request_stop(int) { stopping = 1; }
}

int main(int argc, char** argv) {
    if (argc != 4) { std::cerr << "usage: voice_demo_ui HOST PORT WAKE_MODELS_DIRECTORY\n"; return 2; }
    bool valid = false;
    const auto port = QString::fromLocal8Bit(argv[2]).toUInt(&valid);
    if (!valid || port == 0 || port > 65535) { std::cerr << "invalid gRPC port\n"; return 2; }
    struct sigaction action{};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, nullptr) != 0 || sigaction(SIGINT, &action, nullptr) != 0) return 1;
    QGuiApplication app(argc, argv);
    // 尚未通过官方 CLI 的授权/输出/独占播放器 gate，不注入猜测协议或生产测试替身。
    MusicController music;
    WakeMicrophone wake(QString::fromLocal8Bit(argv[3]));
    VoiceClient client(QString::fromLocal8Bit(argv[1]), static_cast<quint16>(port), music, wake);
    QTimer shutdown_timer;
    QObject::connect(&shutdown_timer, &QTimer::timeout, &client, [&] {
        if (!stopping) return;
        shutdown_timer.stop();
        // signal handler 只写标记；在 GUI 线程请求 Exit、等待真实清理和网络 CQ 收敛。
        client.requestClose();
    });
    shutdown_timer.start(50);
    QObject::connect(&client, &VoiceClient::closeReady, &app, &QCoreApplication::quit);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("voiceClient", &client);
    engine.rootContext()->setContextProperty("musicController", &music);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    client.connectBackend();
    std::cout << "voice_demo_ui ready transport=grpc" << std::endl;
    return app.exec();
}
