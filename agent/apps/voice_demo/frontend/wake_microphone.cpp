#include "wake_microphone.hpp"
#include "wake_engine.hpp"
#include "pcm_frame_buffer.hpp"

#include <QAudioSource>
#include <QAudioDevice>
#include <QElapsedTimer>
#include <QMediaDevices>
#include <QTimer>
#include <array>

class WakeMicrophone::Worker final : public QObject {
public:
    Worker(WakeMicrophone& owner, QString models) : owner_(owner), models_(std::move(models)) {}
    void start(quint64 epoch) {
        halt();
        current_ = epoch;
        if (!valid()) return;
        try {
            if (!engine_) engine_ = std::make_unique<WakeEngine>(models_.toStdString());
            else engine_->reset();
        } catch (const std::exception& error) {
            fail(QString::fromUtf8(error.what())); return;
        }
        if (!valid()) return;
        const auto device = QMediaDevices::defaultAudioInput();
        if (device.isNull() || device.description().contains("monitor", Qt::CaseInsensitive)) {
            fail("未找到麦克风输入，拒绝使用输出回环设备"); return;
        }
        QAudioFormat format;
        format.setSampleRate(WakeEngine::kSampleRate);
        format.setChannelCount(1);
        format.setSampleFormat(QAudioFormat::Int16);
        if (!device.isFormatSupported(format)) {
            fail("麦克风不支持 16 kHz mono PCM16；本版不猜测格式或自动转换"); return;
        }
        source_ = std::make_unique<QAudioSource>(device, format);
        source_->setBufferSize(32000);
        connect(source_.get(), &QAudioSource::stateChanged, this, [this, source = source_.get(), epoch](QAudio::State) {
            if (epoch == current_ && source_.get() == source && valid() && source_->error() != QAudio::NoError)
                fail("麦克风采集错误：" + QString::number(source_->error()));
        }, Qt::QueuedConnection);
        input_ = source_->start();
        if (!input_ || source_->error() != QAudio::NoError) {
            fail("麦克风启动失败，请检查 Windows/WSLg 麦克风权限"); return;
        }
        connect(input_, &QIODevice::readyRead, this, [this] { drain(); });
        data_clock_.start();
        timer_ = std::make_unique<QTimer>();
        connect(timer_.get(), &QTimer::timeout, this, [this] {
            if (!valid()) { halt(); return; }
            if (data_clock_.elapsed() >= 2000) { fail("麦克风连续 2 秒无数据，监听已停止"); return; }
            if (command_clock_.isValid()) handle(engine_->checkDeadline(static_cast<int>(command_clock_.elapsed())));
        });
        timer_->start(50);
        emit owner_.listening(current_, device.description());
    }
    void stop(quint64 epoch) {
        if (owner_.epoch_.load() != epoch) return;
        halt(); emit owner_.settled(epoch);
    }
    void close() { halt(); engine_.reset(); QThread::currentThread()->quit(); }
private:
    bool valid() const { return !owner_.closing_.load() && owner_.epoch_.load() == current_; }
    void halt() {
        if (timer_) { timer_->stop(); timer_.reset(); }
        if (source_) {
            source_->disconnect(this);
            if (input_) input_->disconnect(this);
            source_->stop(); source_.reset(); input_ = nullptr;
        }
        pending_.clear(); command_clock_.invalidate();
    }
    void fail(QString error) {
        const auto epoch = current_;
        halt();
        if (valid()) emit owner_.failed(epoch, error);
    }
    void handle(WakeEngine::Result result) {
        if (!valid()) { halt(); return; }
        switch (result.event) {
        case WakeEngine::Event::kWake:
            command_clock_.start(); emit owner_.awake(current_); break;
        case WakeEngine::Event::kSpeech: emit owner_.speech(current_); break;
        case WakeEngine::Event::kCommand: {
            const auto epoch = current_;
            halt();
            if (valid()) emit owner_.wavReady(epoch, QByteArray::fromStdString(result.wav));
            break;
        }
        case WakeEngine::Event::kFailed: fail(QString::fromStdString(result.error)); break;
        case WakeEngine::Event::kCancelled: {
            const auto epoch = current_;
            halt();
            if (valid()) emit owner_.cancelled(epoch, QString::fromStdString(result.error));
            break;
        }
        default: break;
        }
    }
    void drain() {
        if (!input_) return;
        if (!valid()) { halt(); return; }
        if (input_->bytesAvailable() < 0 || input_->bytesAvailable() > static_cast<qint64>(PcmFrameBuffer::kCapacity - pending_.size())) {
            fail("采音积压超过 1 秒，未提交不完整指令"); return;
        }
        const auto bytes = input_->read(static_cast<qint64>(PcmFrameBuffer::kCapacity - pending_.size()));
        if (bytes.isEmpty()) return;
        data_clock_.restart();
        if (!pending_.append(bytes.constData(), static_cast<std::size_t>(bytes.size()))) { fail("PCM 缓冲超限"); return; }
        std::array<float, WakeEngine::kFrameSamples> frame{};
        while (input_ && pending_.pop(frame)) {
            if (!valid()) { halt(); return; }
            try {
                handle(engine_->accept(frame.data(), static_cast<int>(frame.size()), [this] {
                    return command_clock_.isValid() ? static_cast<int>(command_clock_.elapsed()) : 0;
                }));
            }
            catch (const std::exception& error) { fail("唤醒推理失败：" + QString::fromUtf8(error.what())); }
        }
    }
    WakeMicrophone& owner_;
    QString models_;
    quint64 current_{0};
    std::unique_ptr<WakeEngine> engine_;
    std::unique_ptr<QAudioSource> source_;
    QIODevice* input_{nullptr};
    std::unique_ptr<QTimer> timer_;
    PcmFrameBuffer pending_;
    QElapsedTimer data_clock_, command_clock_;
};

WakeMicrophone::WakeMicrophone(QString models) : worker_(new Worker(*this, std::move(models))) {
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(&thread_, &QThread::finished, this, [this] { closed_ = true; emit closed(); });
    thread_.start();
}
WakeMicrophone::~WakeMicrophone() {
    requestClose();
    // 正常 UI 退出异步等 finished；这里只是异常栈退出的 RAII 兜底，不遗留采音线程。
    thread_.wait();
}
void WakeMicrophone::listen(quint64 epoch) {
    if (closing_.load()) return;
    epoch_.store(epoch);
    QMetaObject::invokeMethod(worker_, [this, epoch] { worker_->start(epoch); }, Qt::QueuedConnection);
}
void WakeMicrophone::stop(quint64 epoch) {
    if (closing_.load()) return;
    // 先使在途推理失去提交权限，再投递停止；GUI 不等待模型调用返回。
    epoch_.store(epoch);
    QMetaObject::invokeMethod(worker_, [this, epoch] { worker_->stop(epoch); }, Qt::QueuedConnection);
}
void WakeMicrophone::requestClose() {
    if (closing_.exchange(true)) return;
    QMetaObject::invokeMethod(worker_, [this] { worker_->close(); }, Qt::QueuedConnection);
}
