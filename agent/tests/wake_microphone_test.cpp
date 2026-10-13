#include "wake_microphone.hpp"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }
template <class Ready> bool wait(Ready ready) {
    QElapsedTimer clock; clock.start();
    while (!ready() && clock.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return ready();
}
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    try {
        if (argc == 2 && QString(argv[1]) == "--devices") {
            QAudioFormat format; format.setSampleRate(16000); format.setChannelCount(1); format.setSampleFormat(QAudioFormat::Int16);
            for (const auto& device : QMediaDevices::audioInputs())
                std::cout << device.description().toStdString() << " default=" << device.isDefault() << " pcm16_16k=" << device.isFormatSupported(format) << '\n';
            return 0; // 只枚举，不在测试中偷偷开启真实麦克风。
        }
        WakeMicrophone missing("/cabinflow-test-missing-models");
        bool failed = false, closed = false, submitted = false;
        QObject::connect(&missing, &WakeInput::failed, &app, [&](quint64 epoch, const QString& detail) {
            require(epoch == 1 && detail.contains("缺失"), "missing model reports matching generation"); failed = true;
        });
        QObject::connect(&missing, &WakeInput::closed, &app, [&] { closed = true; });
        QObject::connect(&missing, &WakeInput::wavReady, &app, [&] { submitted = true; });
        missing.listen(1); require(wait([&] { return failed; }), "model error delivered without starting audio");
        require(!submitted && !missing.isClosed(), "failure is not fake success or thread completion");
        missing.requestClose(); require(wait([&] { return closed && missing.isClosed(); }), "close waits actual worker thread finish");
        WakeMicrophone cancelled("/cabinflow-test-missing-models");
        cancelled.listen(1); cancelled.stop(2); cancelled.requestClose();
        require(wait([&] { return cancelled.isClosed(); }), "close wins queued model load/listen/stop and remains responsive");
        QTemporaryDir corrupt; require(corrupt.isValid(), "owned temporary model copy");
        std::ifstream manifest(CABINFLOW_WAKE_TEST_MANIFEST);
        std::string hash, relative;
        int files = 0;
        while (manifest >> hash >> relative) {
            const auto target = corrupt.path() + "/" + QString::fromStdString(relative);
            require(QDir().mkpath(QFileInfo(target).absolutePath()), "copy parent");
            require(QFile::copy(QString(CABINFLOW_WAKE_TEST_MODELS) + "/" + QString::fromStdString(relative), target), "copy fixed models");
            ++files;
        }
        require(files == 5, "five expected files");
        QFile damaged(corrupt.path() + "/silero_vad_v5.onnx");
        require(damaged.open(QIODevice::ReadWrite) && damaged.write("bad", 3) == 3, "damage only test copy"); damaged.close();
        WakeMicrophone invalid(corrupt.path()); bool rejected = false;
        QObject::connect(&invalid, &WakeInput::failed, &app, [&](quint64 epoch, const QString& detail) {
            require(epoch == 1 && detail.contains("SHA-256"), "corrupt model rejected before SDK exit branch"); rejected = true;
        });
        invalid.listen(1); require(wait([&] { return rejected; }), "corrupted model failure is observable, no fake success");
        invalid.requestClose(); require(wait([&] { return invalid.isClosed(); }), "corrupt model cleanup joins real worker");
        std::cout << "wake_microphone_test passed (no microphone recording performed)\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
