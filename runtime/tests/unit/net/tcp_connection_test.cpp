#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <cabinflow/net/event_loop.hpp>
#include <cabinflow/net/tcp_connection.hpp>

#include "acceptor.hpp"

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FileDescriptor final {
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                static_cast<void>(::close(value_));
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return value_; }

    void close() {
        if (value_ >= 0) {
            if (::close(value_) < 0) {
                throw std::system_error(errno, std::generic_category(), "close");
            }
            value_ = -1;
        }
    }

private:
    int value_{-1};
};

class LoopThread final {
public:
    LoopThread() : thread_([this] {
        cabinflow::net::EventLoop loop;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop_ = &loop;
            ready_ = true;
        }
        changed_.notify_all();
        loop.loop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop_ = nullptr;
        }
        changed_.notify_all();
    }) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return ready_; });
    }

    ~LoopThread() {
        if (thread_.joinable()) {
            loop().quit();
            thread_.join();
        }
    }

    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;

    template <typename Task>
    void run_and_wait(Task&& task) {
        auto completion = std::make_shared<std::promise<void>>();
        std::future<void> completed = completion->get_future();
        loop().queue_in_loop([completion, task = std::forward<Task>(task)]() mutable {
            try {
                task();
                completion->set_value();
            } catch (...) {
                completion->set_exception(std::current_exception());
            }
        });
        completed.get();
    }

    [[nodiscard]] cabinflow::net::EventLoop& loop() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loop_ == nullptr) {
            throw std::logic_error("EventLoop thread is not running");
        }
        return *loop_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    cabinflow::net::EventLoop* loop_{nullptr};
    bool ready_{false};
    std::thread thread_;
};

struct Observations {
    std::mutex mutex;
    std::condition_variable changed;
    std::string received;
    std::vector<bool> connection_states;
    bool closed{false};
};

struct ServerHandle {
    std::shared_ptr<cabinflow::net::detail::Acceptor> acceptor;
    std::uint16_t port{0};
};

ServerHandle start_server(LoopThread& loop_thread, Observations& observations,
                          bool close_from_message_callback) {
    ServerHandle server;
    loop_thread.run_and_wait([&] {
        auto acceptor = std::make_shared<cabinflow::net::detail::Acceptor>(
            loop_thread.loop(), cabinflow::net::detail::InetAddress::loopback(0), false);
        acceptor->set_new_connection_callback(
            [&loop_thread, &observations, close_from_message_callback](
                cabinflow::net::detail::Socket&& socket,
                const cabinflow::net::detail::InetAddress&) {
                const auto connection = cabinflow::net::TcpConnection::create(
                    loop_thread.loop(), "tcp-connection-test", socket.release());
                connection->set_connection_callback([&observations](
                                                        const cabinflow::net::TcpConnectionPtr& value) {
                    std::lock_guard<std::mutex> lock(observations.mutex);
                    observations.connection_states.push_back(value->connected());
                    observations.changed.notify_all();
                });
                connection->set_message_callback(
                    [&observations, close_from_message_callback](
                        const cabinflow::net::TcpConnectionPtr& value, std::string_view bytes) {
                        {
                            std::lock_guard<std::mutex> lock(observations.mutex);
                            observations.received.append(bytes);
                            observations.changed.notify_all();
                        }
                        if (close_from_message_callback) {
                            value->force_close();
                        } else {
                            value->send("echo:" + std::string(bytes));
                        }
                    });
                connection->set_close_callback([&observations](
                                                   const cabinflow::net::TcpConnectionPtr&) {
                    std::lock_guard<std::mutex> lock(observations.mutex);
                    observations.closed = true;
                    observations.changed.notify_all();
                });
                connection->establish();
            });
        acceptor->listen();
        server.port = acceptor->bound_port();
        server.acceptor = std::move(acceptor);
    });
    return server;
}

void stop_server(LoopThread& loop_thread, ServerHandle* server) {
    if (!server->acceptor) {
        return;
    }
    loop_thread.run_and_wait([server] {
        server->acceptor->stop();
        server->acceptor.reset();
    });
}

FileDescriptor connect_client(std::uint16_t port) {
    FileDescriptor client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    if (client.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "client socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(client.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::system_error(errno, std::generic_category(), "connect");
    }
    return client;
}

void send_all(int file_descriptor, const std::string& bytes) {
    std::size_t sent = 0;
    while (sent != bytes.size()) {
        const ssize_t result = ::send(file_descriptor, bytes.data() + sent,
                                      bytes.size() - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::system_error(errno, std::generic_category(), "send");
    }
}

std::string read_exact(int file_descriptor, std::size_t expected_size) {
    std::string result;
    result.resize(expected_size);
    std::size_t received = 0;
    while (received != expected_size) {
        pollfd ready{file_descriptor, POLLIN, 0};
        require(::poll(&ready, 1, 1'000) == 1 && (ready.revents & POLLIN) != 0,
                "client did not receive response");
        const ssize_t count = ::recv(file_descriptor, result.data() + received,
                                     expected_size - received, 0);
        require(count > 0, "connection closed before complete response");
        received += static_cast<std::size_t>(count);
    }
    return result;
}

bool wait_for(Observations& observations,
              const std::function<bool(const Observations&)>& predicate) {
    std::unique_lock<std::mutex> lock(observations.mutex);
    return observations.changed.wait_for(lock, 1s, [&] { return predicate(observations); });
}

void test_peer_close_notifies_lifecycle_after_response() {
    LoopThread loop_thread;
    Observations observations;
    ServerHandle server = start_server(loop_thread, observations, false);
    try {
        FileDescriptor client = connect_client(server.port);
        send_all(client.get(), "cabinflow");
        require(read_exact(client.get(), std::string("echo:cabinflow").size()) ==
                    "echo:cabinflow",
                "unexpected response payload");
        client.close();

        require(wait_for(observations, [](const Observations& value) { return value.closed; }),
                "peer close did not invoke close callback");
        std::lock_guard<std::mutex> lock(observations.mutex);
        require(observations.received == "cabinflow", "message callback lost TCP bytes");
        require(observations.connection_states == std::vector<bool>({true, false}),
                "connection callback lifecycle is invalid");
    } catch (...) {
        stop_server(loop_thread, &server);
        throw;
    }
    stop_server(loop_thread, &server);
}

void test_force_close_from_message_callback_is_safe() {
    LoopThread loop_thread;
    Observations observations;
    ServerHandle server = start_server(loop_thread, observations, true);
    try {
        FileDescriptor client = connect_client(server.port);
        send_all(client.get(), "close-now");

        require(wait_for(observations, [](const Observations& value) { return value.closed; }),
                "force_close did not invoke close callback");
        std::lock_guard<std::mutex> lock(observations.mutex);
        require(observations.received == "close-now", "message callback lost bytes before close");
        require(observations.connection_states == std::vector<bool>({true, false}),
                "callback close lifecycle is invalid");
    } catch (...) {
        stop_server(loop_thread, &server);
        throw;
    }
    stop_server(loop_thread, &server);
}

}  // namespace

int main() {
    try {
        test_peer_close_notifies_lifecycle_after_response();
        test_force_close_from_message_callback_is_safe();
        std::cout << "tcp connection test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tcp connection test failed: " << error.what() << '\n';
        return 1;
    }
}
