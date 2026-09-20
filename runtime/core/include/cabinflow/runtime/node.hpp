#pragma once

#include <string_view>

#include <cabinflow/runtime/node_context.hpp>
#include <cabinflow/runtime/runtime_error.hpp>

namespace cabinflow::runtime {

class Node {
public:
    virtual ~Node() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual RuntimeError start(NodeContext& context) = 0;
    virtual void stop() noexcept = 0;
};

}  // namespace cabinflow::runtime
