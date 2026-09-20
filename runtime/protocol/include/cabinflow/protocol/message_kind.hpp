#pragma once

namespace cabinflow::protocol {

enum class MessageKind {
    kData,
    kCancel,
    kError,
};

}  // namespace cabinflow::protocol
