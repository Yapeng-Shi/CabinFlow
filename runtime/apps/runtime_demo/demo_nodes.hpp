#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string_view>

#include <cabinflow/observability/logger.hpp>
#include <cabinflow/runtime/clock.hpp>
#include <cabinflow/runtime/node.hpp>
#include <cabinflow/runtime/node_context.hpp>
#include <cabinflow/runtime/runtime_error.hpp>
#include <cabinflow/runtime/session_ledger.hpp>
#include <cabinflow/transport/transport.hpp>

namespace cabinflow::demo {

class TextSourceNode final : public runtime::Node {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] runtime::RuntimeError start(
        runtime::NodeContext& context) override;
    void stop() noexcept override;

    [[nodiscard]] transport::TransportError publish(std::string_view text);

private:
    runtime::NodeContext* context_{nullptr};
};

class EchoProcessorNode final : public runtime::Node {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] runtime::RuntimeError start(
        runtime::NodeContext& context) override;
    void stop() noexcept override;
    [[nodiscard]] std::size_t completed_count() const noexcept;

private:
    void on_message(const protocol::Message& message);

    runtime::NodeContext* context_{nullptr};
    std::unique_ptr<transport::Subscription> subscription_;
    std::atomic<std::size_t> completed_count_{0};
};

}  // namespace cabinflow::demo
