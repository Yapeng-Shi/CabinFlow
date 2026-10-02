#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <cabinflow/observability/logger.hpp>
#include <cabinflow/protocol/message.hpp>
#include <cabinflow/runtime/runtime.hpp>
#include <cabinflow/transport/in_memory/in_memory_transport.hpp>

#include "fake_clock.hpp"

namespace {

using cabinflow::runtime::Runtime;
using cabinflow::runtime::RuntimeError;
using namespace std::chrono_literals;

void require(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "expectation failed: " << description << '\n';
        // 死锁时不能靠析构 join 退出；失败直接终止测试进程，不把超时当作通过。
        std::_Exit(1);
    }
}

class Watchdog final {
public:
    Watchdog() : worker_([this] {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, 15s, [this] { return finished_; }),
                "lifecycle test timed out (possible deadlock)");
    }) {}

    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        changed_.notify_all();
        worker_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool finished_{false};
    std::thread worker_;
};

class Signal final {
public:
    void set() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            set_ = true;
        }
        changed_.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, 5s, [this] { return set_; }),
                "lifecycle barrier timed out");
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_{false};
};

class NullLogger final : public cabinflow::observability::Logger {
public:
    void log(const cabinflow::observability::Event&) override {}
};

cabinflow::protocol::Message make_message(std::string id,
                                          std::uint64_t sequence) {
    cabinflow::protocol::Message message;
    auto& envelope = message.envelope;
    envelope.message_id = std::move(id);
    envelope.trace_id = "trace";
    envelope.session_id = "session";
    envelope.work_id = "work";
    envelope.source_node = "source";
    envelope.target_node = "target";
    envelope.topic = "lifecycle.input";
    envelope.sequence = sequence;
    envelope.created_monotonic_ns = 1'000'000;
    envelope.ttl_ms = 1'000;
    return message;
}

struct TargetState {
    Signal handler_entered;
    Signal query_allowed;
    Signal queries_done;
    Signal handler_finish;
    Signal stop_entered;
    Signal stop_finish;
    std::atomic<unsigned> handled{0};
    std::atomic<unsigned> stopped{0};
    std::atomic<bool> handler_completed{false};
    std::atomic<bool> node_stopped{false};
    std::atomic<unsigned> failure_callbacks{0};
    std::atomic<unsigned> completion_callbacks{0};
};

class ReentrantTarget final : public cabinflow::runtime::TargetNode {
public:
    ReentrantTarget(Runtime& runtime, TargetState& state)
        : runtime_(runtime), state_(state) {}

    std::string_view name() const noexcept override { return "target"; }

    RuntimeError start(cabinflow::runtime::NodeContext& context) override {
        context_ = &context;
        require(!runtime_.running(), "start callback can query Runtime");
        return RuntimeError::kNone;
    }

    void stop() noexcept override {
        require(!runtime_.running(), "stop callback can query Runtime");
        require(state_.handler_completed.load(), "worker joined before node stop");
        ++state_.stopped;
        state_.stop_entered.set();
        state_.stop_finish.wait();
        state_.node_stopped = true;
    }

    cabinflow::runtime::MessageHandlingResult on_message(
        const cabinflow::protocol::Message&) noexcept override {
        ++state_.handled;
        state_.handler_entered.set();
        state_.query_allowed.wait();
        // handler 被固定在 stop 已关闭准入之后，查询不能被 join 所持的锁阻塞。
        require(!runtime_.running(), "worker observes stopped admission");
        require(runtime_.reserve_target(name()).error ==
                    Runtime::TargetReservationError::kNotRunning,
                "worker reservation rejected during shutdown");
        require(runtime_.target_stats(name()).has_value(),
                "worker can read stats during shutdown");
        state_.queries_done.set();
        state_.handler_finish.wait();
        state_.handler_completed = true;
        return cabinflow::runtime::MessageHandlingResult::kUnsupportedTopic;
    }

    cabinflow::runtime::DeliveryResult observe(
        const cabinflow::protocol::MessageEnvelope& envelope) {
        return context_->ledger().observe(envelope);
    }

private:
    Runtime& runtime_;
    TargetState& state_;
    cabinflow::runtime::NodeContext* context_{nullptr};
};

void test_shutdown_and_reservation() {
    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    NullLogger logger;
    TargetState state;
    Runtime runtime(transport, clock, logger);
    auto target = std::make_unique<ReentrantTarget>(runtime, state);
    auto* target_ptr = target.get();
    require(runtime.add_target_node(std::move(target), 2) == RuntimeError::kNone,
            "register target");
    require(runtime.start() == RuntimeError::kNone, "start target runtime");
    auto first = runtime.reserve_target("target");
    require(runtime.admit_reserved(
                std::move(first.reservation), make_message("executing", 0),
                [&](Runtime::TargetDeliveryFailure failure) {
                    require(failure == Runtime::TargetDeliveryFailure::kUnsupportedTopic,
                            "delivery failure callback receives handler result");
                    require(!runtime.running(), "failure callback can query Runtime");
                    ++state.failure_callbacks;
                },
                [&] {
                    require(runtime.target_stats("target").has_value(),
                            "completion callback can query Runtime");
                    ++state.completion_callbacks;
                }).admitted,
            "admit executing job");
    state.handler_entered.wait();
    auto queued = runtime.reserve_target("target");
    require(runtime.admit_reserved(std::move(queued.reservation),
                                  make_message("queued", 1), {}).admitted,
            "admit queued job");
    auto held = runtime.reserve_target("target");
    require(static_cast<bool>(held), "hold a reservation across stop");

    std::atomic<bool> stop_returned{false};
    std::thread stopping([&] {
        runtime.stop();
        require(state.node_stopped.load(), "stop returns after node release");
        stop_returned = true;
    });
    // running 是公开的准入状态；watchdog 将锁环或不推进状态判为失败。
    while (runtime.running()) {
        std::this_thread::yield();
    }
    const auto rejected = make_message("after-stop", 2);
    require(!runtime.admit_reserved(std::move(held.reservation), rejected, {}).admitted,
            "reservation cannot commit after stop admission closes");
    require(target_ptr->observe(rejected.envelope) ==
                cabinflow::runtime::DeliveryResult::kAccepted,
            "rejected commit did not mutate ledger");
    require(runtime.start() == RuntimeError::kLifecycleEnded,
            "start during shutdown rejected");
    state.query_allowed.set();
    state.queries_done.wait();
    require(!stop_returned.load(), "stop waits for executing handler");
    state.handler_finish.set();
    state.stop_entered.wait();

    std::atomic<bool> second_returned{false};
    std::thread second_stop([&] {
        runtime.stop();
        require(state.node_stopped.load(), "concurrent stop waits for full cleanup");
        second_returned = true;
    });
    // 等真正选中等待分支后才放行清理，不能用“即将调用 stop”的信号代替。
    while (runtime.metric_value("runtime.stop_waits") != 1) {
        std::this_thread::yield();
    }
    require(!second_returned.load(), "waiting stop has not returned before release");
    state.stop_finish.set();
    second_stop.join();
    stopping.join();
    require(state.handled == 1, "queued job discarded before node stop");
    require(state.stopped == 1, "concurrent callers stop node only once");
    require(state.failure_callbacks == 1 && state.completion_callbacks == 1,
            "executing job callbacks finish before stop returns");
    require(runtime.start() == RuntimeError::kLifecycleEnded,
            "stopped target runtime cannot restart");
    require(runtime.add_target_node(
                std::make_unique<ReentrantTarget>(runtime, state), 1) ==
                RuntimeError::kLifecycleEnded,
            "terminal runtime rejects target registration");
}

class StartupNode final : public cabinflow::runtime::Node {
public:
    StartupNode(Runtime& runtime, std::vector<std::string_view>& events,
                std::string_view name, bool throws, Signal* entered = nullptr,
                Signal* release = nullptr)
        : runtime_(runtime), events_(events), name_(name), throws_(throws),
          entered_(entered), release_(release) {}

    std::string_view name() const noexcept override { return name_; }

    RuntimeError start(cabinflow::runtime::NodeContext&) override {
        require(!runtime_.running(), "startup callback query does not deadlock");
        events_.push_back(name_);
        if (entered_) {
            entered_->set();
            release_->wait();
        }
        if (throws_) {
            throw std::runtime_error("explicit startup failure");
        }
        return RuntimeError::kNone;
    }

    void stop() noexcept override {
        require(!runtime_.running(), "rollback callback query does not deadlock");
        events_.push_back(name_);
    }

private:
    Runtime& runtime_;
    std::vector<std::string_view>& events_;
    std::string_view name_;
    bool throws_;
    Signal* entered_;
    Signal* release_;
};

void test_exception_rollback() {
    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    NullLogger logger;
    std::vector<std::string_view> events;
    Runtime runtime(transport, clock, logger);
    for (const auto name : {"first", "second", "throwing"}) {
        require(runtime.add_node(std::make_unique<StartupNode>(
                    runtime, events, name, name == std::string_view("throwing"))) ==
                    RuntimeError::kNone,
                "register startup nodes");
    }
    bool caught = false;
    try {
        static_cast<void>(runtime.start());
    } catch (const std::runtime_error& error) {
        caught = error.what() == std::string_view("explicit startup failure");
    }
    require(caught, "original startup exception is propagated");
    require(events == std::vector<std::string_view>{
                          "first", "second", "throwing", "second", "first"},
            "rollback reverses only successfully started prefix");
    require(runtime.start() == RuntimeError::kLifecycleEnded,
            "exception consumes the one-shot instance");
    runtime.stop();
    runtime.stop();
    require(events.size() == 5, "failed instance is not stopped twice");
}

void test_stop_during_start(bool throws) {
    cabinflow::test::FakeClock clock(1'000'000);
    cabinflow::transport::InMemoryTransport transport;
    NullLogger logger;
    std::vector<std::string_view> events;
    Signal start_entered;
    Signal finish_start;
    Runtime runtime(transport, clock, logger);
    require(runtime.add_node(std::make_unique<StartupNode>(
                runtime, events, "node", throws, &start_entered, &finish_start)) ==
                RuntimeError::kNone,
            "register blocked startup node");
    std::thread starting([&] {
        try {
            require(runtime.start() == RuntimeError::kNone && !throws,
                    "original startup succeeds");
        } catch (const std::runtime_error& error) {
            require(throws && error.what() == std::string_view("explicit startup failure"),
                    "startup failure is preserved with stop waiter");
        }
    });
    start_entered.wait();
    require(runtime.start() == RuntimeError::kAlreadyStarted,
            "concurrent start rejected while starting");
    require(!runtime.running(), "Starting is not an admission-ready state");
    std::atomic<bool> stop_returned{false};
    std::thread stopping([&] {
        runtime.stop();
        require(events.size() == (throws ? 1U : 2U),
                "stop returns only after startup or failure rollback");
        stop_returned = true;
    });
    while (runtime.metric_value("runtime.stop_waits") != 1) {
        std::this_thread::yield();
    }
    require(!stop_returned.load(), "stop waiter has not returned during startup");
    finish_start.set();
    starting.join();
    stopping.join();
    require(runtime.start() == RuntimeError::kLifecycleEnded,
            "start/stop race leaves terminal instance");
}

}  // namespace

int main() {
    Watchdog watchdog;
    test_shutdown_and_reservation();
    test_exception_rollback();
    test_stop_during_start(false);
    test_stop_during_start(true);
}
