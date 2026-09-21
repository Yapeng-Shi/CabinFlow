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

#include <cabinflow/net/event_loop.hpp>
#include <cabinflow/net/tcp_server.hpp>

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
    std::size_t connected_count{0};
    std::size_t disconnected_count{0};
};

struct ServerHandle {
    std::unique_ptr<cabinflow::net::TcpServer> server;
    std::uint16_t port{0};
};

ServerHandle start_server(LoopThread& loop_thread, Observations& observations) {
    ServerHandle handle;
    loop_thread.run_and_wait([&] {
        auto server = std::make_unique<cabinflow::net::TcpServer>(
            loop_thread.loop(), "tcp-server-test", "127.0.0.1", 0);
        server->set_worker_count(2);
        server->set_connection_callback([&observations](
                                            const cabinflow::net::TcpConnectionPtr& connection) {
            std::lock_guard<std::mutex> lock(observations.mutex);
            if (connection->connected()) {
                ++observations.connected_count;
            } else {
                ++observations.disconnected_count;
            }
            observations.changed.notify_all();
        });
        server->set_message_callback([&observations](
                                         const cabinflow::net::TcpConnectionPtr& connection,
                                         std::string_view bytes) {
            {
                std::lock_guard<std::mutex> lock(observations.mutex);
                observations.received.append(bytes);
                observations.changed.notify_all();
            }
            connection->send(std::string(bytes));
        });
        server->start();
        handle.port = server->bound_port();
        handle.server = std::move(server);
    });
    return handle;
}

void stop_server(LoopThread& loop_thread, ServerHandle* handle) {
    if (!handle->server) {
        return;
    }
    loop_thread.run_and_wait([handle] {
        handle->server->stop();
        require(handle->server->connection_count() == 0,
                "TcpServer retained a connection after stop");
        handle->server.reset();
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
    std::string result(expected_size, '\0');
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

void test_multiple_connections_and_stop_release() {
    LoopThread loop_thread;
    Observations observations;
    ServerHandle server = start_server(loop_thread, observations);
    try {
        FileDescriptor first_client = connect_client(server.port);
        FileDescriptor second_client = connect_client(server.port);
        send_all(first_client.get(), "ab");
        send_all(first_client.get(), "cd");
        send_all(second_client.get(), "xy");
        send_all(second_client.get(), "z");
        require(read_exact(first_client.get(), 4) == "abcd", "first response lost TCP bytes");
        require(read_exact(second_client.get(), 3) == "xyz", "second response lost TCP bytes");
        require(wait_for(observations, [](const Observations& value) {
                    return value.connected_count == 2 && value.received.size() == 7;
                }),
                "server did not process both connections");

        first_client.close();
        require(wait_for(observations, [](const Observations& value) {
                    return value.disconnected_count == 1;
                }),
                "peer close did not remove the first server connection");

        stop_server(loop_thread, &server);
        pollfd closed{second_client.get(), POLLIN | POLLHUP, 0};
        require(::poll(&closed, 1, 1'000) == 1 &&
                    (closed.revents & (POLLIN | POLLHUP)) != 0,
                "server stop did not close the remaining client socket");
        char byte = '\0';
        require(::recv(second_client.get(), &byte, sizeof(byte), 0) == 0,
                "server stop did not release the remaining TCP connection");

        std::lock_guard<std::mutex> lock(observations.mutex);
        require(observations.disconnected_count == 2,
                "server stop did not publish the final disconnect callback");
    } catch (...) {
        stop_server(loop_thread, &server);
        throw;
    }
}

}  // namespace

int main() {
    try {
        test_multiple_connections_and_stop_release();
        std::cout << "tcp server test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tcp server test failed: " << error.what() << '\n';
        return 1;
    }
}
