#include "voice_client.hpp"
#include "music_test_backend.hpp"
#include <cockpit_task.pb.h>
#include <control.pb.h>
#include <delivery.pb.h>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using cabinflow::protocol::Message;
using cabinflow::protocol::MessageKind;
void require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }
bool wait_until(const std::function<bool()>& ready, int timeout_ms = 2000) {
    QElapsedTimer elapsed; elapsed.start();
    while (!ready() && elapsed.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}
class Fixture {
public:
    explicit Fixture(std::unique_ptr<MusicBackend> backend = {}) : music(std::move(backend)) {
        require(server.listen(QHostAddress::LocalHost, 0), "listen Qt contract peer");
        client = std::make_unique<VoiceClient>("127.0.0.1", server.serverPort(), music);
        client->connectBackend();
        require(wait_until([this] { return server.hasPendingConnections() && client->connected(); }), "Qt socket connects asynchronously");
        peer = server.nextPendingConnection();
        QObject::connect(peer, &QTcpSocket::readyRead, peer, [this] {
            const auto bytes = peer->readAll();
            auto decoded = framer.feed(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
            require(static_cast<bool>(decoded), "Qt client sends valid length-prefix Protobuf");
            for (auto& message : decoded.messages) incoming.push_back(std::move(message));
        });
    }
    Message take() {
        require(wait_until([this] { return !incoming.empty(); }), "Qt peer receives request");
        auto message = std::move(incoming.front()); incoming.pop_front(); return message;
    }
    Message response(const Message& request, const char* topic) {
        auto message = request;
        message.envelope.message_id = "test-response-" + std::to_string(++sequence);
        std::swap(message.envelope.source_node, message.envelope.target_node);
        message.envelope.topic = topic; message.envelope.sequence = sequence;
        message.envelope.is_final = true; message.envelope.kind = MessageKind::kData;
        return message;
    }
    void send(const Message& message, bool fragment = false) {
        const auto encoded = cabinflow::gateway::RuntimeMessageFramer::encode(message);
        require(static_cast<bool>(encoded), "encode contract peer response");
        if (fragment) {
            peer->write(encoded.bytes.data(), 2); peer->flush();
            const QByteArray tail(encoded.bytes.data() + 2, static_cast<qsizetype>(encoded.bytes.size() - 2));
            QTimer::singleShot(5, peer, [this, tail] { peer->write(tail); });
        } else peer->write(encoded.bytes.data(), static_cast<qint64>(encoded.bytes.size()));
    }
    void setup(const Message& request, const char* work = "test-work") {
        auto message = response(request, "control.response"); message.envelope.work_id = work;
        cabinflow::protocol::v1::ControlRequest original;
        require(original.ParseFromString(request.payload) && original.has_setup(), "initial request is Setup");
        cabinflow::protocol::v1::ControlResponse result; result.set_request_message_id(request.envelope.message_id);
        auto* info = result.mutable_setup()->mutable_work();
        info->set_work_id(work); info->set_unit_id(original.setup().unit_id()); info->set_state(cabinflow::protocol::v1::WORK_STATE_RUNNING);
        message.payload = result.SerializeAsString(); send(message, true);
    }
    void exited(const Message& request, bool invalid_inner = false, int error = 0) {
        auto message = response(request, "control.response");
        cabinflow::protocol::v1::ControlResponse result; result.set_request_message_id(request.envelope.message_id);
        if (error) { result.mutable_error()->set_code(static_cast<std::uint32_t>(error)); result.mutable_error()->set_message("already exited"); message.envelope.kind = MessageKind::kError; }
        else {
            auto* info = result.mutable_exit()->mutable_work();
            info->set_work_id(invalid_inner ? "wrong-work" : request.envelope.work_id);
            info->set_unit_id("dialogue.primary"); info->set_state(cabinflow::protocol::v1::WORK_STATE_EXITED);
        }
        message.payload = result.SerializeAsString(); send(message);
    }
    void rejected(const Message& input) {
        auto message = response(input, "runtime.delivery.error"); message.envelope.kind = MessageKind::kError;
        cabinflow::protocol::v1::DeliveryError result;
        result.set_request_message_id(input.envelope.message_id); result.set_code(cabinflow::protocol::v1::DELIVERY_ERROR_QUEUE_FULL);
        result.set_message("test queue full"); message.payload = result.SerializeAsString(); send(message);
    }
    void cancelled(const Message& input, bool climate_on, bool applied, bool window_open) {
        auto message = response(input, "cockpit.task.result"); message.envelope.kind = MessageKind::kError;
        cabinflow::agent::v1::VoiceTaskResult result; result.set_request_message_id(input.envelope.message_id);
        result.mutable_vehicle()->set_simulated(true);
        result.mutable_vehicle()->set_climate_on(climate_on);
        result.mutable_vehicle()->set_left_front_window_open(window_open);
        result.mutable_vehicle()->set_action_applied(applied);
        result.mutable_cancelled()->set_reason("test cancellation after SDK return"); message.payload = result.SerializeAsString(); send(message);
    }
    void failed(const Message& input, bool climate_on, bool applied, bool window_open) {
        auto message = response(input, "cockpit.task.result"); message.envelope.kind = MessageKind::kError;
        cabinflow::agent::v1::VoiceTaskResult result; result.set_request_message_id(input.envelope.message_id);
        result.mutable_vehicle()->set_simulated(true);
        result.mutable_vehicle()->set_climate_on(climate_on);
        result.mutable_vehicle()->set_left_front_window_open(window_open);
        result.mutable_vehicle()->set_action_applied(applied);
        result.mutable_failed()->set_message("test TTS failure"); message.payload = result.SerializeAsString(); send(message);
    }
    void audio(const Message& input, const QByteArray& wav, bool window_open) {
        auto message = response(input, "cockpit.task.result");
        cabinflow::agent::v1::VoiceTaskResult result; result.set_request_message_id(input.envelope.message_id);
        result.set_answer("测试回答"); result.mutable_vehicle()->set_simulated(true);
        result.mutable_vehicle()->set_left_front_window_open(window_open);
        result.mutable_audio()->set_request_message_id(input.envelope.message_id);
        result.mutable_audio()->set_wav_bytes(wav.constData(), static_cast<std::size_t>(wav.size()));
        message.payload = result.SerializeAsString(); send(message);
    }
    MusicController music;
    QTcpServer server;
    QTcpSocket* peer{nullptr};
    std::unique_ptr<VoiceClient> client;
    cabinflow::gateway::RuntimeMessageFramer framer;
    std::deque<Message> incoming;
    std::uint64_t sequence{0};
};
void setup_cancel_and_close() {
    Fixture f; f.client->startText("问题"); const auto setup = f.take();
    f.client->cancel(); f.setup(setup);
    const auto exit = f.take(); cabinflow::protocol::v1::ControlRequest body;
    require(body.ParseFromString(exit.payload) && body.has_exit(), "cancelled Setup emits Exit without data");
    require(f.client->busy(), "Setup cancellation waits for cleanup");
    f.exited(exit); require(wait_until([&] { return !f.client->busy(); }), "Setup cleanup does not wait for nonexistent TaskResult");
    require(f.client->status() == "已取消" && f.incoming.empty(), "no business input after cancelled Setup");
    bool close_ready = false; QObject::connect(f.client.get(), &VoiceClient::closeReady, [&] { close_ready = true; });
    f.client->requestClose(); require(close_ready, "idle GUI closes without blocking");
}
void cancel_then_admission_rejection() {
    for (int cleanup_error : {0, 5, 6}) {
        Fixture f; f.client->startText("问题"); f.setup(f.take()); const auto input = f.take();
        f.client->cancel(); const auto exit = f.take();
        bool close_ready = false; QObject::connect(f.client.get(), &VoiceClient::closeReady, [&] { close_ready = true; });
        f.client->requestClose(); f.rejected(input);
        require(wait_until([&] { return f.client->error() == "test queue full"; }), "admission rejection received after cancel");
        f.exited(exit, false, cleanup_error);
        require(wait_until([&] { return close_ready && !f.client->busy(); }), "pending Exit upgraded to pure cleanup without waiting forever");
        require(f.client->status() == "失败" && !f.client->hasAudio() && f.incoming.empty(), "rejection remains failure with exactly one Exit");
    }
}
void task_result_before_exit_and_responsive() {
    Fixture f; f.client->startText("问题"); f.setup(f.take()); const auto input = f.take();
    f.client->cancel(); const auto exit = f.take();
    int timer_ticks = 0; QTimer timer; timer.setInterval(1);
    QObject::connect(&timer, &QTimer::timeout, [&] { ++timer_ticks; }); timer.start();
    require(wait_until([&] { return timer_ticks >= 5; }), "GUI event loop remains responsive while cancelling");
    require(f.client->busy(), "Exit request alone cannot prove handler cleanup");
    f.cancelled(input, false, false, false); require(wait_until([&] { return !f.client->busy(); }), "business terminal proves cleanup");
    // 下一任务换成 ASR Unit；旧 Exit 必须按原请求身份验证，不能使用新的当前 unit。
    QTemporaryFile wav; require(wav.open(), "create temporary input"); wav.write("test-wav"); wav.flush();
    f.client->startWav(QUrl::fromLocalFile(wav.fileName())); const auto next_setup = f.take();
    f.exited(exit); require(wait_until([&] { return f.peer->bytesToWrite() == 0; }), "deliver late ExitResponse");
    QCoreApplication::processEvents();
    require(f.client->busy() && f.client->connected() && f.client->status() == "创建任务", "old Exit cannot revive cancellation or fail new Unit");
    f.setup(next_setup, "next-work"); const auto next_input = f.take();
    require(next_input.envelope.topic == "cockpit.audio.input", "explicit WAV entry uses typed audio topic");
    f.cancelled(next_input, false, false, false); require(wait_until([&] { return !f.client->busy(); }), "new work has independent result association");
}
void invalid_cleanup_and_disconnect() {
    Fixture f; f.client->startText("问题"); const auto setup = f.take(); f.client->cancel(); f.setup(setup);
    const auto exit = f.take(); f.exited(exit, true);
    require(wait_until([&] { return !f.client->connected(); }), "wrong inner WorkInfo rejected and connection closed");
    require(f.client->status() != "已取消", "wrong cleanup response never claims handler/work cleaned");
    Fixture lost; lost.client->startText("问题"); static_cast<void>(lost.take()); lost.peer->disconnectFromHost();
    require(wait_until([&] { return !lost.client->connected() && !lost.client->busy(); }), "disconnect surfaced");
    require(lost.client->error().contains("未知"), "disconnect is unknown backend state, not cancellation success");
    lost.client->startText("不能自动重连"); require(!lost.server.hasPendingConnections(), "no hidden reconnect or inference fallback");
}
void qml_window() {
    Fixture f; QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("voiceClient", f.client.get());
    engine.rootContext()->setContextProperty("musicController", &f.music);
    engine.load(QUrl("qrc:/Main.qml")); require(!engine.rootObjects().isEmpty(), "one-screen QML window loads without component errors");
    f.client->requestClose();
}
void vehicle_receipt_contract() {
    Fixture f;
    require(!f.client->vehicleKnown() && f.client->vehicleState().contains("未知"), "frontend does not assume backend initial state");
    f.client->startText("打开空调"); f.setup(f.take()); const auto on_input = f.take();
    f.failed(on_input, true, true, false);
    require(wait_until([&] { return !f.client->busy(); }), "TTS failure has a final task outcome");
    require(f.client->vehicleKnown() && f.client->climateOn() && f.client->vehicleAction().contains("已执行") &&
        f.client->status() == "失败" && !f.client->hasAudio(), "speech failure preserves executed simulation fact without audio");
    f.client->startText("关闭空调"); f.setup(f.take(), "second-work"); const auto off_input = f.take();
    require(f.client->vehicleState().contains("最近回执") && f.client->climateOn() && f.client->vehicleAction().contains("等待"),
            "new task marks last confirmed state and waits for new action receipt");
    f.failed(on_input, false, true, false);  // 旧 work 的迟到回执不能改当前显示。
    f.cancelled(off_input, true, false, false);
    require(wait_until([&] { return !f.client->busy(); }), "cancelled task receives independent current receipt");
    require(f.client->vehicleKnown() && f.client->climateOn() && f.client->vehicleAction().contains("未执行") &&
        f.client->status() == "已取消" && !f.client->hasAudio(), "pre-action cancellation leaves previous state unchanged");
    f.client->startText("关闭空调"); f.setup(f.take(), "third-work"); const auto applied_input = f.take();
    f.cancelled(applied_input, false, true, false);
    require(wait_until([&] { return !f.client->busy(); }), "post-action cancellation receives a receipt");
    require(f.client->vehicleKnown() && !f.client->climateOn() && f.client->vehicleAction().contains("已执行") &&
        f.client->answer().isEmpty() && !f.client->hasAudio(), "post-action cancellation retains state but suppresses old answer/audio");
    f.peer->disconnectFromHost();
    require(wait_until([&] { return !f.client->connected(); }) && !f.client->vehicleKnown(), "disconnect invalidates last vehicle state");
    for (bool missing : {true, false}) {
        Fixture invalid; invalid.client->startText("问题"); invalid.setup(invalid.take()); const auto input = invalid.take();
        auto message = invalid.response(input, "cockpit.task.result"); message.envelope.kind = MessageKind::kError;
        cabinflow::agent::v1::VoiceTaskResult result; result.set_request_message_id(input.envelope.message_id);
        result.mutable_failed()->set_message("failed");
        if (!missing) result.mutable_vehicle()->set_simulated(false);
        message.payload = result.SerializeAsString(); invalid.send(message);
        require(wait_until([&] { return !invalid.client->connected(); }) && !invalid.client->vehicleKnown(),
                "missing receipt or false simulation identity rejected, not guessed as OFF");
    }
}
void answer_audio_path_contract() {
    // 一个 PCM16 样本的真实 WAV 容器；本测试只验证文件所有权，不声称人工可听。
    const auto wav = QByteArray::fromHex("524946462600000057415645666d74201000000001000100803e0000007d0000020010006461746102000000e803");
    QTemporaryDir archive; require(archive.isValid(), "temporary archive directory");
    Fixture f; require(f.client->answerAudioPath().isEmpty(), "no historical audio before result");
    auto complete = [&] {
        f.client->startText("问题"); f.setup(f.take()); const auto input = f.take(); f.audio(input, wav, false);
        require(wait_until([&] { return !f.client->busy(); }), "successful typed audio result");
        const auto path = f.client->answerAudioPath(); QFile file(path);
        require(!path.isEmpty() && file.open(QIODevice::ReadOnly) && file.readAll() == wav, "getter exposes current complete WAV");
        return path;
    };
    const auto first = complete(); const auto copy = archive.filePath("answer.wav");
    require(QFile::copy(first, copy), "synchronous independent archive copy");
    f.client->stopPlayback(); require(f.client->answerAudioPath() == first, "stop retains completed answer");
    f.client->startText("下一任务");
    require(f.client->answerAudioPath().isEmpty() && !QFile::exists(first), "new task deletes prior temporary WAV");
    f.setup(f.take()); f.cancelled(f.take(), false, false, false);
    require(wait_until([&] { return !f.client->busy(); }) && f.client->answerAudioPath().isEmpty(), "cancel has no audio path");
    const auto second = complete(); f.peer->disconnectFromHost();
    require(wait_until([&] { return !f.client->connected(); }) && f.client->answerAudioPath().isEmpty() && !QFile::exists(second), "disconnect clears owned audio");
    QFile saved(copy); require(saved.open(QIODevice::ReadOnly) && saved.readAll() == wav, "archive survives source deletion");
    Fixture closing; closing.client->startText("问题"); closing.setup(closing.take()); closing.audio(closing.take(), wav, false);
    require(wait_until([&] { return !closing.client->busy(); }), "close fixture has audio");
    const auto third = closing.client->answerAudioPath(); closing.client->requestClose();
    require(closing.client->answerAudioPath().isEmpty() && !QFile::exists(third), "close releases audio");
    Fixture destroyed; destroyed.client->startText("问题"); destroyed.setup(destroyed.take()); destroyed.audio(destroyed.take(), wav, false);
    require(wait_until([&] { return !destroyed.client->busy(); }), "destruction fixture has audio");
    const auto fourth = destroyed.client->answerAudioPath(); destroyed.client.reset();
    require(!QFile::exists(fourth), "client destruction releases temporary audio");
}
void cockpit_scene_contract() {
    Fixture f; QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("voiceClient", f.client.get());
    engine.rootContext()->setContextProperty("musicController", &f.music);
    engine.load(QUrl("qrc:/Main.qml")); require(!engine.rootObjects().isEmpty(), "cockpit screen loads");
    auto* scene = engine.rootObjects().front()->findChild<QObject*>("cockpitScene");
    require(scene && !scene->property("stateKnown").toBool() && !scene->property("glassVisible").toBool() &&
        !scene->property("breezeVisible").toBool(), "scene starts unknown, not CLOSED");
    f.client->startText("打开左前车窗"); f.setup(f.take()); const auto first = f.take();
    require(scene->property("pending").toBool() && !scene->property("stateKnown").toBool(), "no optimistic window state before receipt");
    f.failed(first, true, true, true);
    require(wait_until([&] { return !f.client->busy(); }) && scene->property("stateKnown").toBool() &&
        scene->property("leftFrontWindowOpen").toBool() && scene->property("climateOn").toBool(), "TTS failure still visualizes trusted executed facts");
    require(wait_until([&] { return scene->property("glassLevel").toDouble() < 0.01; }) && scene->property("breezeVisible").toBool(),
        "confirmed OPEN lowers glass; confirmed AC enables breeze");
    f.client->startText("打开左前车窗"); f.setup(f.take(), "window-second"); const auto second = f.take();
    require(scene->property("pending").toBool() && scene->property("leftFrontWindowOpen").toBool(), "pending preserves last confirmed window state");
    f.failed(second, true, true, true);
    require(wait_until([&] { return !f.client->busy(); }) && scene->property("leftFrontWindowOpen").toBool(), "repeated OPEN does not toggle");
    f.client->startText("关闭左前车窗"); f.setup(f.take(), "window-third"); const auto third = f.take();
    f.failed(first, false, true, false); f.cancelled(third, true, false, true);
    require(wait_until([&] { return !f.client->busy(); }) && scene->property("leftFrontWindowOpen").toBool() &&
        scene->property("climateOn").toBool(), "late old work and pre-action cancellation cannot change current view");
    for (int index = 0; index < 2; ++index) {
        f.client->startText("关闭左前车窗"); f.setup(f.take(), index ? "window-fifth" : "window-fourth");
        f.failed(f.take(), true, true, false);
        require(wait_until([&] { return !f.client->busy(); }) && !scene->property("leftFrontWindowOpen").toBool() &&
            scene->property("climateOn").toBool(), "repeated CLOSE leaves AC unchanged");
        require(wait_until([&] { return scene->property("glassLevel").toDouble() > 0.99; }), "confirmed CLOSE settles glass up");
    }
    f.peer->disconnectFromHost();
    require(wait_until([&] { return !f.client->connected(); }) && !scene->property("stateKnown").toBool() &&
        !scene->property("glassVisible").toBool() && !scene->property("breezeVisible").toBool(), "disconnect renders UNKNOWN, hides glass and breeze");
}
void invalid_vehicle_result_never_updates_view() {
    for (int kind = 0; kind < 5; ++kind) {
        bool forged_seen = false;
        Fixture f; QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("voiceClient", f.client.get());
        engine.rootContext()->setContextProperty("musicController", &f.music);
        engine.load(QUrl("qrc:/Main.qml")); require(!engine.rootObjects().isEmpty(), "invalid-result scene loads");
        auto* scene = engine.rootObjects().front()->findChild<QObject*>("cockpitScene"); require(scene, "scene available");
        QObject observer;
        QObject::connect(f.client.get(), &VoiceClient::changed, &observer, [&] {
            forged_seen = forged_seen || f.client->vehicleKnown() || scene->property("stateKnown").toBool();
        });
        f.client->startText("打开左前车窗"); f.setup(f.take()); const auto input = f.take();
        auto message = f.response(input, "cockpit.task.result");
        cabinflow::agent::v1::VoiceTaskResult result; result.set_request_message_id(input.envelope.message_id);
        result.mutable_vehicle()->set_simulated(true); result.mutable_vehicle()->set_climate_on(true);
        result.mutable_vehicle()->set_action_applied(true);
        // 0..2 分别缺失成功/失败/取消的窗口字段；3/4 是合法车辆回执但非法音频关联/内容。
        if (kind >= 3) result.mutable_vehicle()->set_left_front_window_open(true);
        if (kind == 1) { result.mutable_failed()->set_message("failure"); message.envelope.kind = MessageKind::kError; }
        else if (kind == 2) { result.mutable_cancelled()->set_reason("cancelled"); message.envelope.kind = MessageKind::kError; }
        else {
            result.mutable_audio()->set_request_message_id(kind == 3 ? "wrong-request" : input.envelope.message_id);
            if (kind != 4) result.mutable_audio()->set_wav_bytes("bytes");
        }
        message.payload = result.SerializeAsString(); f.send(message);
        require(wait_until([&] { return !f.client->connected(); }) && !forged_seen && !f.client->vehicleKnown() &&
            !scene->property("stateKnown").toBool(), "invalid current message never exposes forged vehicle state on any notification");
    }
}
QByteArray read_file(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "open evidence input");
    const auto bytes = file.readAll(); require(file.error() == QFileDevice::NoError, "read evidence input"); return bytes;
}
QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QJsonObject json_object(const QByteArray& bytes) {
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(), "valid JSON object"); return doc.object();
}
void save_json(const QString& path, const QJsonObject& value) {
    QSaveFile file(path); const auto bytes = QJsonDocument(value).toJson();
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "commit current evidence report");
}
QString normalize_transcript(QString text) {
    static const QRegularExpression removed("[\\p{P}\\s]", QRegularExpression::UseUnicodePropertiesOption);
    return text.remove(removed);
}
int cases_probe(quint16 port, const QString& cases_path, const QString& output_dir) {
    const auto manifest_bytes = read_file(cases_path); const auto manifest = json_object(manifest_bytes);
    require(QFileInfo(manifest["registered_inputs"].toString()).canonicalFilePath() == QFileInfo(CABINFLOW_REGISTERED_INPUTS).canonicalFilePath(),
        "use repository registered inputs, not an alternate expectation file");
    const auto registered_bytes = read_file(CABINFLOW_REGISTERED_INPUTS);
    require(sha256(registered_bytes) == manifest["registered_inputs_sha256"].toString(), "registered input hash unchanged");
    const auto registered = json_object(registered_bytes)["cases"].toArray(); const auto cases = manifest["cases"].toArray();
    const QStringList ids{"greeting", "arithmetic", "capital", "ac_on", "ac_off"};
    require(cases.size() == 5 && registered.size() == 5, "exactly five registered cases");
    for (qsizetype index = 0; index < cases.size(); ++index) {
        const auto entry = cases[index].toObject(); const auto original = registered[index].toObject();
        require(entry["id"].toString() == ids[index], "fixed case IDs and order");
        for (const auto* key : {"id", "text", "reference_text", "expected_result"})
            require(entry[key].isString() && entry[key] == original[key], "do not rewrite registered expectations");
        const auto input = entry["input_audio"].toObject();
        require(input["sample_rate"].toInt() == 16000 && input["channels"].toInt() == 1 && input["sample_width"].toInt() == 2 &&
            sha256(read_file(entry["wav_path"].toString())) == input["sha256"].toString(), "pinned PCM16 WAV input");
    }
    require(QDir().mkdir(output_dir), "create new archive directory only (parent must exist)");
    const auto report_path = QDir(output_dir).filePath("report.json");
    QJsonArray trials;
    QJsonObject report{{"status", "running"}, {"cases_sha256", sha256(manifest_bytes)}, {"manifest", manifest},
        {"qt_version", qVersion()}, {"platform", QGuiApplication::platformName()}, {"concurrency", 1},
        {"human_listening", "not_verified"}, {"answer_semantics", "not_verified"},
        {"probe_binary_sha256", sha256(read_file(QCoreApplication::applicationFilePath()))}};
    save_json(report_path, report);
    MusicController music;
    VoiceClient client("127.0.0.1", port, music);
    QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("voiceClient", &client);
    engine.rootContext()->setContextProperty("musicController", &music);
    engine.load(QUrl("qrc:/Main.qml")); require(!engine.rootObjects().isEmpty(), "cases QML window loads");
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    require(window && wait_until([&] { return window->isExposed(); }, 10000), "cases window exposed");
    client.connectBackend(); require(wait_until([&] { return client.connected(); }, 10000), "cases backend connection");
    int execution_count = 0, exact_count = 0, vehicle_count = 0, playing_count = 0;
    bool observed_failure = false;
    for (const auto& value : cases) for (int repeat = 1; repeat <= 3; ++repeat) {
        const auto entry = value.toObject(); const auto id = entry["id"].toString();
        const auto trial_dir = QDir(output_dir).filePath(id + "-" + QString::number(repeat));
        require(QDir().mkdir(trial_dir), "new pertrial archive");
        QJsonObject trial{{"id", id}, {"repeat", repeat}, {"reference_text", entry["reference_text"]},
            {"expected_result", entry["expected_result"]}, {"status", "started"}, {"human_listening", "not_verified"}};
        trials.append(trial); report["trials"] = trials; save_json(report_path, report);
        QElapsedTimer input_timer; input_timer.start(); client.startWav(QUrl::fromLocalFile(entry["wav_path"].toString()));
        const bool finished = wait_until([&] { return !client.busy(); }, 180000);
        const auto input_to_final_ns = input_timer.nsecsElapsed();
        trial["status"] = client.status(); trial["error"] = client.error(); trial["transcript"] = client.transcript();
        trial["answer"] = client.answer(); trial["vehicle_known"] = client.vehicleKnown();
        trial["vehicle_state"] = client.vehicleState(); trial["vehicle_action"] = client.vehicleAction();
        if (client.vehicleKnown()) {
            trial["climate_on"] = client.climateOn();
            trial["left_front_window_open"] = client.leftFrontWindowOpen();
        }
        if (!finished) {
            trial["status"] = "timeout"; trials.replace(trials.size() - 1, trial);
            report["trials"] = trials; report["status"] = "incomplete_timeout"; save_json(report_path, report);
            client.requestClose();
            require(wait_until([&] { return !client.busy(); }, 180000), "timed out task cleanup not confirmed");
            return 1;  // 未清理前绝不发下一条；超时不伪装为成功或自动重试。
        }
        const bool execution = client.status() == "已完成" && client.hasAudio() && client.error().isEmpty();
        trial["execution_completed"] = execution;
        const bool exact = normalize_transcript(client.transcript()) == normalize_transcript(entry["reference_text"].toString());
        trial["asr_reference_match"] = exact; exact_count += exact;
        const bool applied = client.vehicleAction() == "本次动作：已执行（模拟）";
        const bool not_applied = client.vehicleAction() == "本次动作：未执行";
        if (applied || not_applied) trial["action_applied"] = applied;
        const bool climate_case = id == "ac_on" || id == "ac_off";
        const bool vehicle_ok = client.vehicleKnown() && (climate_case ? applied && client.climateOn() == (id == "ac_on") : not_applied);
        trial["vehicle_expectation_match"] = vehicle_ok;
        if (climate_case) vehicle_count += vehicle_ok;
        if (execution) {
            ++execution_count; trial["start_to_local_wav_complete_ns"] = input_to_final_ns;
            const auto saved_wav = QDir(trial_dir).filePath("answer.wav");
            // 先同步复制，再泵事件/开始播放；异步断连或下一任务可能删除客户端的临时文件。
            require(QFile::copy(client.answerAudioPath(), saved_wav), "copy current answer WAV without ownership transfer");
            trial["answer_wav"] = saved_wav; trial["answer_wav_sha256"] = sha256(read_file(saved_wav));
        }
        trials.replace(trials.size() - 1, trial); report["trials"] = trials; save_json(report_path, report);
        if (execution) {
            bool seen_playing = false; qint64 playing_ns = 0; QElapsedTimer playback_timer;
            QObject observer;  // 回调 context 先析构，避免捕获的局部计时变量悬空。
            QObject::connect(&client, &VoiceClient::changed, &observer, [&] {
                if (client.playing() && !seen_playing) { seen_playing = true; playing_ns = playback_timer.nsecsElapsed(); }
            });
            playback_timer.start(); client.play();
            wait_until([&] { return seen_playing || !client.error().isEmpty(); }, 10000);
            trial["playing_state_observed"] = seen_playing; trial["playback_error"] = client.error();
            if (seen_playing) { ++playing_count; trial["play_command_to_PlayingState_ns"] = playing_ns; }
            const auto screenshot = QDir(trial_dir).filePath("ui.png");
            require(window->grabWindow().save(screenshot), "save actual Qt screenshot");
            const bool playing_before_cancel = client.playing();
            client.cancel(); require(!client.playing(), "idle playback cancel stops player");
            trial["playing_before_cancel"] = playing_before_cancel;
            trial["playback_cancel_stopped"] = playing_before_cancel;
        }
        observed_failure = observed_failure || !execution || !exact || !vehicle_ok ||
            !trial["playing_state_observed"].toBool() || !trial["playback_error"].toString().isEmpty();
        trials.replace(trials.size() - 1, trial); report["trials"] = trials; save_json(report_path, report);
        std::cout << "cases id=" << id.toStdString() << " repeat=" << repeat << " execution=" << execution
                  << " asr_match=" << exact << " vehicle_match=" << vehicle_ok << '\n' << std::flush;
        if (!client.connected()) { report["status"] = "incomplete_disconnect"; save_json(report_path, report); return 1; }
    }
    report["execution_completed"] = execution_count; report["asr_reference_matches"] = exact_count;
    report["climate_action_matches"] = vehicle_count; report["playing_state_observed"] = playing_count;
    report["status"] = observed_failure ? "failed_observed_contracts" : "awaiting_human_quality";
    save_json(report_path, report); client.requestClose();
    return observed_failure ? 1 : 0;
}
void live_probe(quint16 port, const QString& wav_path, const QString& output_dir) {
    MusicController music;
    VoiceClient client("127.0.0.1", port, music);
    QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("voiceClient", &client);
    engine.rootContext()->setContextProperty("musicController", &music);
    engine.load(QUrl("qrc:/Main.qml")); require(!engine.rootObjects().isEmpty(), "live WSLg QML window loads");
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    require(window && wait_until([&] { return window->isExposed(); }, 10000), "live window is exposed by WSLg");
    std::cout << "live Qt platform=" << QGuiApplication::platformName().toStdString() << '\n' << std::flush;
    client.connectBackend(); require(wait_until([&] { return client.connected(); }, 10000), "live backend connection");
    require(!client.vehicleKnown(), "live frontend waits for actual state receipt");
    if (!output_dir.isEmpty()) {
        const auto path = QDir(output_dir).filePath("unknown-ui.png");
        require(!QFile::exists(path) && window->grabWindow().save(path), "capture unknown before any receipt");
    }
    for (bool audio : {false, true}) {
        if (audio && wav_path.isEmpty()) continue;
        QElapsedTimer input_timer; input_timer.start();
        if (audio) client.startWav(QUrl::fromLocalFile(wav_path)); else client.startText("中国的首都是哪里？");
        require(client.busy(), "live task starts");
        int ticks = 0; QTimer timer; timer.setInterval(10); QObject::connect(&timer, &QTimer::timeout, [&] { ++ticks; }); timer.start();
        require(wait_until([&] { return !client.busy(); }, 180000), "live inference final result");
        const auto input_to_final_ns = input_timer.nsecsElapsed();
        require(client.hasAudio() && client.error().isEmpty() && !client.answer().isEmpty(), "real final result contains answer WAV");
        std::cout << "live entry=" << (audio ? "wav" : "text") << " transcript=" << client.transcript().toStdString()
                  << " answer=" << client.answer().toStdString() << " ui_timer_ticks=" << ticks
                  << " vehicle_known=" << client.vehicleKnown() << " climate_on=" << client.climateOn()
                  << " left_front_window_open=" << client.leftFrontWindowOpen()
                  << " action=" << client.vehicleAction().toStdString()
                  << " start_to_local_wav_complete_ns=" << input_to_final_ns << '\n';
        QElapsedTimer playback_timer; playback_timer.start();
        client.play(); require(wait_until([&] { return client.playing() || !client.error().isEmpty(); }, 10000), "playback begins");
        require(client.playing() && client.error().isEmpty(), "WSLg player reaches PlayingState");
        std::cout << "live play_command_to_PlayingState_ns=" << playback_timer.nsecsElapsed() << '\n';
        if (!output_dir.isEmpty()) {
            const auto path = QDir(output_dir).filePath(audio ? "wav-ui.png" : "text-ui.png");
            require(!QFile::exists(path) && window->grabWindow().save(path), "save actual QML window without overwriting evidence");
        }
        client.stopPlayback(); require(!client.playing(), "playback stops independently of backend completion");
    }
    for (int index = 0; index < 4; ++index) {
        const bool turn_on = index < 2;
        client.startText(turn_on ? "打开空调" : "关闭空调");
        require(client.busy() && wait_until([&] { return !client.busy(); }, 180000), "real TTS simulation receipt arrives");
        require(client.status() == "已完成" && client.hasAudio() && client.vehicleKnown() &&
            client.climateOn() == turn_on && client.vehicleAction().contains("已执行") && client.answer().contains("模拟"),
            "live set ON/ON/OFF/OFF is not toggle and exposes simulated receipt");
        std::cout << "live FakeVehicle climate_on=" << turn_on << " action_applied=true answer=" << client.answer().toStdString() << '\n';
        client.play(); require(wait_until([&] { return client.playing() || !client.error().isEmpty(); }, 10000), "simulation answer playback starts");
        require(client.playing() && client.error().isEmpty(), "real simulation TTS reaches PlayingState");
        if (!output_dir.isEmpty() && index % 2 == 0) {
            const auto path = QDir(output_dir).filePath(turn_on ? "climate-on-ui.png" : "climate-off-ui.png");
            require(!QFile::exists(path) && window->grabWindow().save(path), "capture simulation receipt without overwriting evidence");
        }
        client.stopPlayback();
    }
    auto* scene = engine.rootObjects().front()->findChild<QObject*>("cockpitScene"); require(scene, "real cockpit scene available");
    for (int index = 0; index < 4; ++index) {
        const bool open = index < 2;
        client.startText(open ? "打开左前车窗" : "关闭左前车窗");
        require(client.busy() && wait_until([&] { return !client.busy(); }, 180000), "real TTS window terminal arrives");
        require(client.status() == "已完成" && client.hasAudio() && client.vehicleKnown() &&
            client.leftFrontWindowOpen() == open && !client.climateOn() && client.vehicleAction().contains("已执行") &&
            client.answer().contains("模拟左前车窗"), "real repeated window sets preserve AC OFF and typed receipt");
        require(wait_until([&] { return open ? scene->property("glassLevel").toDouble() < 0.01 : scene->property("glassLevel").toDouble() > 0.99; }),
            "real scene finishes receipt-driven display transition");
        std::cout << "live FakeVehicle left_front_window_open=" << open << " climate_on=0 action_applied=true\n";
        client.play(); require(wait_until([&] { return client.playing() || !client.error().isEmpty(); }, 10000), "window audio playback starts");
        require(client.playing() && client.error().isEmpty(), "real window TTS reaches PlayingState");
        if (!output_dir.isEmpty() && index % 2 == 0) {
            const auto path = QDir(output_dir).filePath(open ? "window-open-ui.png" : "window-closed-ui.png");
            require(!QFile::exists(path) && window->grabWindow().save(path), "capture actual window state");
        }
        client.cancel(); require(!client.playing(), "window playback cancel stops player");
    }
    for (const auto* command : {"打开空调", "打开左前车窗"}) {
        client.startText(command);
        require(client.busy() && wait_until([&] { return !client.busy(); }, 180000) &&
            client.status() == "已完成" && client.hasAudio() && client.vehicleAction().contains("已执行"), "real combined simulation command");
    }
    require(client.climateOn() && client.leftFrontWindowOpen() && scene->property("breezeVisible").toBool() &&
        wait_until([&] { return scene->property("glassLevel").toDouble() < 0.01; }), "real AC and window states coexist");
    if (!output_dir.isEmpty()) {
        const auto path = QDir(output_dir).filePath("combined-on-ui.png");
        require(!QFile::exists(path) && window->grabWindow().save(path), "capture combined simulation state");
    }
    if (!output_dir.isEmpty()) {
        window->resize(720, 580);
        require(wait_until([&] { return window->width() == 720 && window->height() == 580; }), "minimum window resize");
        QCoreApplication::processEvents();
        const auto path = QDir(output_dir).filePath("minimum-size-ui.png");
        require(!QFile::exists(path) && window->grabWindow().save(path), "capture actual minimum size");
        auto* scroll = window->findChild<QQuickItem*>("interactionScroll");
        auto* controls = window->findChild<QQuickItem*>("playbackControls");
        require(scroll && controls, "minimum-size playback controls exist");
        auto* flick = scroll->property("contentItem").value<QObject*>();
        require(flick && flick->setProperty("contentY", std::max(0.0,
            flick->property("contentHeight").toDouble() - flick->property("height").toDouble())), "scroll to playback controls");
        require(wait_until([&] {
            const auto top = controls->mapToScene(QPointF(0, 0)).y();
            const auto viewport_top = scroll->mapToScene(QPointF(0, 0)).y();
            return top >= viewport_top && top + controls->height() <= viewport_top + scroll->height();
        }), "minimum-size playback controls are reachable by scrolling");
        const auto scrolled_path = QDir(output_dir).filePath("minimum-size-scrolled-ui.png");
        require(!QFile::exists(scrolled_path) && window->grabWindow().save(scrolled_path), "capture reachable playback controls");
    }
    client.startText("中国的首都是哪里？");
    require(wait_until([&] { return client.status() == "处理中"; }), "live cancellation targets an admitted input");
    client.cancel(); require(client.busy() && client.status() == "取消中", "live cancel is a request, not immediate cleanup");
    require(wait_until([&] { return !client.busy(); }, 180000), "live backend cancellation completes");
    require(client.status() == "已取消" && !client.hasAudio() && client.answer().isEmpty(), "live late audio is not retained after cancellation");
    std::cout << "live cancellation=cleaned stale_answer_audio=absent\n";
    bool closed = false; QObject::connect(&client, &VoiceClient::closeReady, [&] { closed = true; });
    client.requestClose(); require(closed, "live window completes close cleanup");
}
void shutdown_probe(quint16 port) {
    bool cancelled_before_disconnect = false;
    MusicController music;
    VoiceClient client("127.0.0.1", port, music);
    QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("voiceClient", &client);
    engine.rootContext()->setContextProperty("musicController", &music);
    engine.load(QUrl("qrc:/Main.qml")); require(!engine.rootObjects().isEmpty(), "shutdown probe window loads");
    client.connectBackend(); require(wait_until([&] { return client.connected(); }, 10000), "shutdown probe connects");
    QObject::connect(&client, &VoiceClient::changed, [&] {
        if (client.status() == "已取消") cancelled_before_disconnect = true;
    });
    client.startText("中国的首都是哪里？");
    require(wait_until([&] { return client.status() == "处理中"; }), "shutdown input sent after real Setup");
    // 外部控制线程/测试操作者发送 SIGTERM；此处只观察，不从 GUI 杀服务或伪造完成。
    std::cout << "shutdown_probe input_sent=1 waiting_for_external_SIGTERM\n" << std::flush;
    require(wait_until([&] { return !client.connected(); }, 60000), "backend signal shutdown closes connection");
    require(!client.busy() && !client.hasAudio() && !client.vehicleKnown() && client.error().contains("未知"),
            "disconnect keeps backend state unknown and clears audio, not a synthetic success");
    std::cout << "shutdown_probe disconnected=1 cancelled_before_disconnect=" << cancelled_before_disconnect << '\n';
}
void music_terminal_contract() {
    auto* backend = new TestMusicBackend;
    Fixture f{std::unique_ptr<MusicBackend>(backend)};
    f.music.search("test"); backend->found({{"encrypted", "1", "测试歌曲", "测试歌手", true}});
    auto result_message = [&](const Message& input) {
        auto message = f.response(input, "cockpit.task.result");
        cabinflow::agent::v1::VoiceTaskResult result;
        result.set_request_message_id(input.envelope.message_id);
        result.mutable_vehicle()->set_simulated(true);
        result.mutable_vehicle()->set_left_front_window_open(false);
        result.mutable_music_command()->set_action(cabinflow::agent::v1::MusicCommand::SELECT);
        result.mutable_music_command()->set_result_index(1);
        message.payload = result.SerializeAsString(); return message;
    };
    f.client->startText("播放第一首"); f.setup(f.take()); const auto first = f.take();
    f.music.search("blocked"); require(backend->calls.size() == 1, "voice submission locks visible list in C++");
    const auto terminal = result_message(first); f.send(terminal); f.send(terminal);
    require(wait_until([&] { return !f.client->busy(); }), "typed non-audio music outcome accepted");
    require(backend->calls.size() == 2 && backend->calls.back().action == "play" && !f.client->hasAudio(),
            "duplicate final result issues one music command, never answer WAV");
    backend->acknowledge(); backend->state(MusicSnapshot::State::kPlaying);
    f.client->startText("下一条"); f.setup(f.take(), "new-music-work"); const auto second = f.take();
    f.send(terminal); f.client->cancel(); static_cast<void>(f.take()); f.send(result_message(second));
    require(wait_until([&] { return !f.client->busy(); }) && backend->calls.size() == 3 &&
        f.client->status().contains("音乐未发起"), "old final and committed-but-locally-cancelled music never replay");
    f.client->startText("关闭前任务"); f.setup(f.take(), "closing-work"); const auto closing_input = f.take();
    bool closed = false; QObject::connect(f.client.get(), &VoiceClient::closeReady, [&] { closed = true; });
    f.client->requestClose(); static_cast<void>(f.take()); f.send(result_message(closing_input));
    require(wait_until([&] { return !f.client->busy(); }) && !closed && backend->shutdown_requested,
            "window waits for both voice cleanup and music cleanup");
    const auto calls = backend->calls.size(); emit backend->closed();
    require(closed && backend->calls.size() == calls, "close proof releases window without new playback");

    Fixture invalid; invalid.client->startText("music"); invalid.setup(invalid.take()); const auto input = invalid.take();
    auto message = invalid.response(input, "cockpit.task.result"); cabinflow::agent::v1::VoiceTaskResult bad;
    bad.set_request_message_id(input.envelope.message_id); bad.mutable_vehicle()->set_simulated(true);
    bad.mutable_vehicle()->set_left_front_window_open(true); bad.mutable_music_command();
    message.payload = bad.SerializeAsString(); invalid.send(message);
    require(wait_until([&] { return !invalid.client->connected(); }) && !invalid.client->vehicleKnown(),
            "invalid typed music rejected before exposing forged receipt");
}

void cockpit_preview(const QString& directory) {
    require(!QFileInfo::exists(directory) && QDir().mkdir(directory), "preview output must be a new directory");
    Fixture f; QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("voiceClient", f.client.get());
    engine.rootContext()->setContextProperty("musicController", &f.music);
    engine.load(QUrl("qrc:/Main.qml")); require(!engine.rootObjects().isEmpty(), "preview QML loads");
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front()); require(window, "preview window");
    auto capture = [&](int width, int height, const QString& filename) {
        window->resize(width, height);
        require(wait_until([&] { return window->isExposed(); }), "preview exposed");
        QElapsedTimer rendering; rendering.start();
        require(wait_until([&] { return rendering.elapsed() >= 200; }), "allow Canvas and layout to settle");
        const auto image = window->grabWindow();
        require(!image.isNull() && image.save(QDir(directory).filePath(filename)), "save actual Qt rendered preview");
    };
    capture(1440, 900, "cockpit-1440.png");
    capture(1080, 740, "cockpit-1080.png");
    capture(720, 580, "cockpit-720.png");
    auto* scroll = window->findChild<QObject*>("cockpitBodyScroll"); require(scroll, "responsive body scroll exists");
    auto* content = scroll->property("contentItem").value<QObject*>(); require(content, "scroll content");
    content->setProperty("contentY", std::max(0.0, content->property("contentHeight").toDouble() - content->property("height").toDouble()));
    capture(720, 580, "cockpit-720-voice.png");
    std::cout << "preview=actual_Qt_render music=disabled vehicle=unknown models=not_run directory="
              << directory.toStdString() << '\n';
    f.client->requestClose();
}
void reentrant_music_cancel_contract() {
    auto* backend = new TestMusicBackend;
    Fixture f{std::unique_ptr<MusicBackend>(backend)};
    f.music.search("test"); backend->found({{"encrypted", "1", "测试歌曲", "测试歌手", true}});
    f.client->startText("播放第一首"); f.setup(f.take()); const auto input = f.take();
    auto message = f.response(input, "cockpit.task.result"); cabinflow::agent::v1::VoiceTaskResult result;
    result.set_request_message_id(input.envelope.message_id); result.mutable_vehicle()->set_simulated(true);
    result.mutable_vehicle()->set_left_front_window_open(false);
    result.mutable_music_command()->set_action(cabinflow::agent::v1::MusicCommand::SELECT);
    result.mutable_music_command()->set_result_index(1); message.payload = result.SerializeAsString();
    bool entered = false; QObject observer;
    QObject::connect(f.client.get(), &VoiceClient::changed, &observer, [&] {
        if (entered) return;
        entered = true;
        f.send(message); f.peer->flush();
        require(wait_until([&] { return !f.client->busy(); }), "cancel notification reenters and receives committed music candidate");
        f.client->startText("下一任务");
    });
    f.client->cancel();
    require(entered && backend->calls.size() == 1 && f.client->status() == "创建任务",
            "cancel is visible before reentrant final and does not cancel the newly started work");
    f.setup(f.take(), "reentrant-new-work"); const auto next = f.take();
    require(next.envelope.topic == "cockpit.text.input", "old cancel cannot turn new Setup into Exit");
    f.cancelled(next, false, false, false);
    require(wait_until([&] { return !f.client->busy(); }), "reentrant next task completes independently");
}
}  // namespace
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    try {
        if (argc == 3 && QString(argv[1]) == "--preview") {
            cockpit_preview(QString(argv[2]));
        } else if (argc == 5 && QString(argv[1]) == "--cases") {
            bool ok = false; const auto port = QString(argv[2]).toUShort(&ok); require(ok && port, "cases probe port");
            return cases_probe(port, QString(argv[3]), QString(argv[4]));
        } else if (argc >= 3 && QString(argv[1]) == "--live") {
            bool ok = false; const auto port = QString(argv[2]).toUShort(&ok); require(ok && port, "live probe port");
            live_probe(port, argc >= 4 ? QString(argv[3]) : QString{}, argc >= 5 ? QString(argv[4]) : QString{});
        } else if (argc == 3 && QString(argv[1]) == "--shutdown") {
            bool ok = false; const auto port = QString(argv[2]).toUShort(&ok); require(ok && port, "shutdown probe port");
            shutdown_probe(port);
        } else {
            require(argc == 1, "usage: --preview NEW_OUTPUT_DIR | --cases PORT CASES_JSON NEW_OUTPUT_DIR | --live PORT [WAV [OUTPUT_DIR]] | --shutdown PORT");
            setup_cancel_and_close(); cancel_then_admission_rejection(); task_result_before_exit_and_responsive();
            invalid_cleanup_and_disconnect(); qml_window(); vehicle_receipt_contract(); answer_audio_path_contract();
            cockpit_scene_contract(); invalid_vehicle_result_never_updates_view();
            music_terminal_contract();
            reentrant_music_cancel_contract();
        }
        std::cout << "voice_client_test passed (human listening not verified)\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
