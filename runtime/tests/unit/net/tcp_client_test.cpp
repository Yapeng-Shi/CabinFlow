#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
#include <cabinflow/net/tcp_client.hpp>
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
    std::string response;
    std::size_t connected_count{0};
    std::size_t disconnected_count{0};
    std::error_code connect_error;
};

bool wait_for(Observations& observations,
              const std::function<bool(const Observations&)>& predicate) {
    std::unique_lock<std::mutex> lock(observations.mutex);
    return observations.changed.wait_for(lock, 1s, [&] { return predicate(observations); });
}

std::uint16_t bound_port(int file_descriptor) {
    sockaddr_in address{};
    socklen_t address_length = sizeof(address);
    if (::getsockname(file_descriptor, reinterpret_cast<sockaddr*>(&address),
                      &address_length) < 0) {
        throw std::system_error(errno, std::generic_category(), "getsockname");
    }
    return ntohs(address.sin_port);
}

FileDescriptor reserve_unlistened_port() {
    FileDescriptor reservation(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    if (reservation.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "reserve socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(reservation.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::system_error(errno, std::generic_category(), "reserve bind");
    }
    return reservation;
}

void test_client_success_callback_stop_and_failure() {
    LoopThread loop_thread;
    Observations success;
    Observations callback_stop;
    Observations failure;
    std::unique_ptr<cabinflow::net::TcpServer> server;
    std::unique_ptr<cabinflow::net::TcpClient> successful_client;
    std::unique_ptr<cabinflow::net::TcpClient> callback_stop_client;
    std::unique_ptr<cabinflow::net::TcpClient> failure_client;
    std::uint16_t server_port = 0;

    loop_thread.run_and_wait([&] {
        server = std::make_unique<cabinflow::net::TcpServer>(
            loop_thread.loop(), "tcp-client-test-server", "127.0.0.1", 0);
        server->set_message_callback([](const cabinflow::net::TcpConnectionPtr& connection,
                                        std::string_view bytes) {
            connection->send(std::string(bytes));
        });
        server->start();
        server_port = server->bound_port();

        successful_client = std::make_unique<cabinflow::net::TcpClient>(
            loop_thread.loop(), "successful-client", "127.0.0.1", server_port);
        successful_client->set_connection_callback([&success](
                                                      const cabinflow::net::TcpConnectionPtr& connection) {
            std::lock_guard<std::mutex> lock(success.mutex);
            if (connection->connected()) {
                ++success.connected_count;
                connection->send("client-payload");
            } else {
                ++success.disconnected_count;
            }
            success.changed.notify_all();
        });
        successful_client->set_message_callback([&success](
                                                   const cabinflow::net::TcpConnectionPtr&,
                                                   std::string_view bytes) {
            std::lock_guard<std::mutex> lock(success.mutex);
            success.response.append(bytes);
            success.changed.notify_all();
        });
        successful_client->set_connect_error_callback([&success](std::error_code error) {
            std::lock_guard<std::mutex> lock(success.mutex);
            success.connect_error = error;
            success.changed.notify_all();
        });
        successful_client->connect();
    });

    try {
        require(wait_for(success, [](const Observations& value) {
                    return value.response == "client-payload";
                }),
                "TcpClient did not complete the successful byte flow");
        {
            std::lock_guard<std::mutex> lock(success.mutex);
            require(success.connected_count == 1 && !success.connect_error,
                    "successful connection callback state is invalid");
        }

        loop_thread.run_and_wait([&] {
            successful_client->stop();
            require(!successful_client->connection(),
                    "TcpClient stop retained a live connection");
            successful_client.reset();

            auto client = std::make_unique<cabinflow::net::TcpClient>(
                loop_thread.loop(), "callback-stop-client", "127.0.0.1", server_port);
            cabinflow::net::TcpClient* const client_pointer = client.get();
            client->set_connection_callback([&callback_stop, client_pointer](
                                                const cabinflow::net::TcpConnectionPtr& connection) {
                const bool connected = connection->connected();
                {
                    std::lock_guard<std::mutex> lock(callback_stop.mutex);
                    if (connected) {
                        ++callback_stop.connected_count;
                    } else {
                        ++callback_stop.disconnected_count;
                    }
                    callback_stop.changed.notify_all();
                }
                if (connected) {
                    client_pointer->stop();
                }
            });
            callback_stop_client = std::move(client);
            callback_stop_client->connect();
        });
        require(wait_for(callback_stop, [](const Observations& value) {
                    return value.connected_count == 1 && value.disconnected_count == 1;
                }),
                "TcpClient stop during connection callback did not close safely");

        loop_thread.run_and_wait([&] { callback_stop_client.reset(); });

        FileDescriptor reservation = reserve_unlistened_port();
        const std::uint16_t unavailable_port = bound_port(reservation.get());
        loop_thread.run_and_wait([&] {
            failure_client = std::make_unique<cabinflow::net::TcpClient>(
                loop_thread.loop(), "failing-client", "127.0.0.1", unavailable_port);
            failure_client->set_connect_error_callback([&failure](std::error_code error) {
                std::lock_guard<std::mutex> lock(failure.mutex);
                failure.connect_error = error;
                failure.changed.notify_all();
            });
            failure_client->connect();
        });
        require(wait_for(failure, [](const Observations& value) {
                    return static_cast<bool>(value.connect_error);
                }),
                "TcpClient did not report a failed connection");
        {
            std::lock_guard<std::mutex> lock(failure.mutex);
            require(failure.connect_error == std::error_code(ECONNREFUSED, std::generic_category()),
                    "TcpClient reported an unexpected connect failure");
        }

        loop_thread.run_and_wait([&] {
            failure_client.reset();
            server->stop();
            server.reset();
        });
    } catch (...) {
        loop_thread.run_and_wait([&] {
            failure_client.reset();
            callback_stop_client.reset();
            successful_client.reset();
            if (server) {
                server->stop();
                server.reset();
            }
        });
        throw;
    }
}

}  // namespace

int main() {
    try {
        test_client_success_callback_stop_and_failure();
        std::cout << "tcp client test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tcp client test failed: " << error.what() << '\n';
        return 1;
    }
}
