#pragma once

#include <functional>
#include <memory>

namespace cabinflow::net {
class EventLoop;
}

namespace cabinflow::net::detail {

class Channel final {
public:
    using Callback = std::function<void()>;

    Channel(EventLoop& loop, int file_descriptor);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    [[nodiscard]] int file_descriptor() const noexcept;
    [[nodiscard]] int events() const noexcept;
    [[nodiscard]] bool is_none_event() const noexcept;
    [[nodiscard]] int index() const noexcept;
    void set_index(int index) noexcept;
    void set_returned_events(int events) noexcept;

    void set_read_callback(Callback callback);
    void set_write_callback(Callback callback);
    void set_close_callback(Callback callback);
    void set_error_callback(Callback callback);
    void tie(const std::shared_ptr<void>& owner);

    void enable_reading();
    void disable_reading();
    void enable_writing();
    void disable_writing();
    void disable_all();
    void remove();
    void handle_events();

private:
    void update();
    void handle_events_with_guard();

    EventLoop& loop_;
    const int file_descriptor_;
    int events_{0};
    int returned_events_{0};
    int index_{-1};
    Callback read_callback_;
    Callback write_callback_;
    Callback close_callback_;
    Callback error_callback_;
    std::weak_ptr<void> owner_;
    bool tied_{false};
};

}  // namespace cabinflow::net::detail
