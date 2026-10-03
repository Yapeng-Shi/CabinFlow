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
    VoiceClient client(QString::fromLocal8Bit(argv[1]), static_cast<quint16>(port));
    QObject::connect(&client, &VoiceClient::closeReady, &app, &QCoreApplication::quit);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("voiceClient", &client);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    client.connectBackend();
    return app.exec();
}
