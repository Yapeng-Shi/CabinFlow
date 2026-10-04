#include "voice_client.hpp"

#include <iostream>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: voice_demo_ui HOST PORT\n"; return 2; }
    bool valid = false;
    const auto port = QString::fromLocal8Bit(argv[2]).toUInt(&valid);
    if (!valid || port == 0 || port > 65535) { std::cerr << "invalid TCP port\n"; return 2; }
    QGuiApplication app(argc, argv);
    // 尚未通过官方 CLI 的授权/输出/独占播放器 gate，不注入猜测协议或生产测试替身。
    MusicController music;
    VoiceClient client(QString::fromLocal8Bit(argv[1]), static_cast<quint16>(port), music);
    QObject::connect(&client, &VoiceClient::closeReady, &app, &QCoreApplication::quit);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("voiceClient", &client);
    engine.rootContext()->setContextProperty("musicController", &music);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    client.connectBackend();
    return app.exec();
}
