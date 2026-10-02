#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace cabinflow::runtime::detail {
struct CancellationState;
}

namespace cabinflow::runtime {

class CancellationRegistry;

enum class CancellationReason {
    kNone,
    kSession,
    kWork,
};

// A token is scoped to one session/work pair. It stays queryable if the
// registry object is destroyed, because in-flight nodes may finish later.
class CancellationToken final {
public:
    CancellationToken() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] CancellationReason reason() const noexcept;

private:
    CancellationToken(std::shared_ptr<detail::CancellationState> state,
                      std::string session_id, std::string work_id);

    std::shared_ptr<detail::CancellationState> state_;
    std::string session_id_;
    std::string work_id_;

    friend class CancellationRegistry;
};

class CancellationRegistry final {
public:
    CancellationRegistry();
    ~CancellationRegistry();

    CancellationRegistry(const CancellationRegistry&) = delete;
    CancellationRegistry& operator=(const CancellationRegistry&) = delete;
    CancellationRegistry(CancellationRegistry&&) = delete;
    CancellationRegistry& operator=(CancellationRegistry&&) = delete;

    [[nodiscard]] CancellationToken token_for(std::string_view session_id,
                                               std::string_view work_id) const;
    [[nodiscard]] bool cancel_session(std::string_view session_id);
    [[nodiscard]] bool cancel_work(std::string_view session_id,
                                   std::string_view work_id);

private:
    std::shared_ptr<detail::CancellationState> state_;
};

}  // namespace cabinflow::runtime
