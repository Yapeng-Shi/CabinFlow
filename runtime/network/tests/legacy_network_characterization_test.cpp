#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <network/Buffer.h>
#include <network/EventLoop.h>
#include <network/EventLoopThread.h>
#include <network/EventLoopThreadPool.h>
#include <network/InetAddress.h>
#include <network/TcpConnection.h>
#include <network/TcpServer.h>

namespace {

using namespace std::chrono_literals;

constexpr auto kOperationTimeout = 2s;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

class FileDescriptor final {
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            shutdown_and_close();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return value_; }

    void shutdown_and_close() noexcept {
        if (value_ >= 0) {
            static_cast<void>(::shutdown(value_, SHUT_RDWR));
            static_cast<void>(::close(value_));
            value_ = -1;
        }
    }

private:
    int value_;
};

std::uint16_t reserve_loopback_port() {
    FileDescriptor socket_fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    require(socket_fd.get() >= 0, "failed to create port-reservation socket");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(socket_fd.get(), reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) == 0,
            "failed to reserve loopback port");

    socklen_t address_size = sizeof(address);
    require(::getsockname(socket_fd.get(),
                          reinterpret_cast<sockaddr*>(&address),
                          &address_size) == 0,
            "failed to read reserved loopback port");
    return ntohs(address.sin_port);
}

FileDescriptor connect_loopback(std::uint16_t port) {
    FileDescriptor socket_fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    require(socket_fd.get() >= 0, "failed to create TCP client socket");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    require(::connect(socket_fd.get(), reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) == 0,
            "failed to connect to legacy TCP server");
    return socket_fd;
}

void send_all(int socket_fd, const std::string& bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto result = ::send(socket_fd, bytes.data() + written,
                                   bytes.size() - written, MSG_NOSIGNAL);
        require(result > 0, "failed to send TCP test bytes");
        written += static_cast<std::size_t>(result);
    }
}

std::string receive_exact(int socket_fd, std::size_t expected_size) {
    std::string result;
    result.reserve(expected_size);
    while (result.size() < expected_size) {
        pollfd descriptor{socket_fd, POLLIN, 0};
        require(::poll(&descriptor, 1, static_cast<int>(kOperationTimeout.count())) > 0,
                "timed out waiting for TCP response");

        char buffer[64];
        const auto received = ::recv(socket_fd, buffer,
                                     std::min(sizeof(buffer), expected_size - result.size()),
                                     0);
        require(received > 0, "peer closed before the expected TCP response");
        result.append(buffer, static_cast<std::size_t>(received));
    }
    return result;
}

void wait_for_peer_close(int socket_fd) {
    pollfd descriptor{socket_fd, POLLIN | POLLHUP, 0};
    require(::poll(&descriptor, 1, static_cast<int>(kOperationTimeout.count())) > 0,
            "timed out waiting for peer close");

    char byte = 0;
    require(::recv(socket_fd, &byte, sizeof(byte), 0) == 0,
            "expected peer close instead of additional TCP data");
}

struct ServerObservations {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t connected_count{0};
    std::size_t disconnected_count{0};
    std::string received_bytes;
};

template <typename Predicate>
void wait_for_observation(ServerObservations& observations, Predicate predicate,
                          const char* description) {
    std::unique_lock<std::mutex> lock(observations.mutex);
    require(observations.changed.wait_for(lock, kOperationTimeout,
                                          [&observations, &predicate] {
                                              return predicate(observations);
                                          }),
            description);
}

class LegacyServerHarness final {
public:
    LegacyServerHarness(std::uint16_t port, ServerObservations& observations)
        : observations_(observations), loop_(loop_thread_.startLoop()) {
        run_on_loop_and_wait([this, port] {
            server_ = std::make_unique<network::TcpServer>(
                loop_, network::InetAddress(port, true), "legacy-characterization");
            server_->setThreadNum(0);
            server_->setConnectionCallback([this](const network::TcpConnectionPtr& connection) {
                std::lock_guard<std::mutex> lock(observations_.mutex);
                if (connection->connected()) {
                    ++observations_.connected_count;
                } else {
                    ++observations_.disconnected_count;
                }
                observations_.changed.notify_all();
            });
            server_->setMessageCallback([this](const network::TcpConnectionPtr& connection,
                                               network::Buffer* buffer) {
                const std::string bytes = buffer->retrieveAllAsString();
                {
                    std::lock_guard<std::mutex> lock(observations_.mutex);
                    observations_.received_bytes.append(bytes);
                    observations_.changed.notify_all();
                }

                // TCP 是字节流，测试命令可能分段到达；仅在收齐命令后触发回调内关闭。
                force_close_candidate_.append(bytes);
                constexpr const char kForceCloseCommand[] = "force-close";
                const std::string command(kForceCloseCommand);
                if (command.compare(0, force_close_candidate_.size(),
                                    force_close_candidate_) == 0) {
                    if (force_close_candidate_ == command) {
                        force_close_candidate_.clear();
                        connection->forceClose();
                    }
                    return;
                }
                force_close_candidate_.clear();

                network::Buffer response;
                response.append(bytes);
                connection->send(&response);
            });
            server_->start();
        });
        started_ = true;
    }

    ~LegacyServerHarness() {
        try {
            stop();
        } catch (...) {
            std::terminate();
        }
    }

    LegacyServerHarness(const LegacyServerHarness&) = delete;
    LegacyServerHarness& operator=(const LegacyServerHarness&) = delete;

    void stop() {
        if (!started_) {
            return;
        }
        run_on_loop_and_wait([this] { server_.reset(); });
        started_ = false;
    }

private:
    template <typename Function>
    void run_on_loop_and_wait(Function&& function) {
        auto completed = std::make_shared<std::promise<void>>();
        auto completion = completed->get_future();
        loop_->queueInLoop([task = std::forward<Function>(function), completed]() mutable {
            try {
                task();
                completed->set_value();
            } catch (...) {
                completed->set_exception(std::current_exception());
            }
        });
        require(completion.wait_for(kOperationTimeout) == std::future_status::ready,
                "timed out waiting for EventLoop task");
        completion.get();
    }

    ServerObservations& observations_;
    network::EventLoopThread loop_thread_;
    network::EventLoop* loop_;
    std::unique_ptr<network::TcpServer> server_;
    std::string force_close_candidate_;
    bool started_{false};
};

void test_buffer_preserves_partial_and_coalesced_bytes() {
    network::Buffer buffer(4);
    buffer.append("ab", 2);
    require(buffer.readableBytes() == 2, "buffer did not retain a partial write");
    buffer.append("cdef", 4);
    require(buffer.retrieveAllAsString() == "abcdef",
            "buffer did not preserve coalesced byte order");
}

void test_event_loop_wakes_for_cross_thread_work() {
    network::EventLoopThread loop_thread;
    network::EventLoop* loop = loop_thread.startLoop();
    auto completed = std::make_shared<std::promise<bool>>();
    auto completion = completed->get_future();

    loop->queueInLoop([loop, completed] {
        completed->set_value(loop->isInLoopThread());
    });
    require(completion.wait_for(kOperationTimeout) == std::future_status::ready,
            "cross-thread queueInLoop did not wake EventLoop");
    require(completion.get(), "queued callback ran outside the EventLoop thread");
}

void test_event_loop_thread_pool_starts_workers_and_rotates() {
    network::EventLoopThread base_thread;
    network::EventLoop* base_loop = base_thread.startLoop();
    auto pool = std::make_shared<std::unique_ptr<network::EventLoopThreadPool>>();
    auto workers = std::make_shared<std::vector<network::EventLoop*>>();
    auto round_robin = std::make_shared<bool>(false);
    auto started = std::make_shared<std::promise<void>>();
    auto start_result = started->get_future();

    base_loop->queueInLoop([base_loop, pool, workers, round_robin, started] {
        try {
            *pool = std::make_unique<network::EventLoopThreadPool>(
                base_loop, "legacy-characterization-pool");
            (*pool)->setThreadNum(2);
            (*pool)->start();
            *workers = (*pool)->getAllLoops();
            network::EventLoop* first = (*pool)->getNextLoop();
            network::EventLoop* second = (*pool)->getNextLoop();
            network::EventLoop* third = (*pool)->getNextLoop();
            *round_robin = first != second && first == third;
            started->set_value();
        } catch (...) {
            started->set_exception(std::current_exception());
        }
    });
    require(start_result.wait_for(kOperationTimeout) == std::future_status::ready,
            "timed out starting legacy EventLoopThreadPool");
    start_result.get();
    require(workers->size() == 2, "thread pool did not expose two worker loops");
    require(*round_robin, "thread pool did not preserve round-robin assignment");

    auto remaining_callbacks = std::make_shared<std::atomic<std::size_t>>(workers->size());
    auto callbacks_ran_in_owner_thread = std::make_shared<std::atomic<bool>>(true);
    auto callbacks_finished = std::make_shared<std::promise<void>>();
    auto callback_result = callbacks_finished->get_future();
    for (network::EventLoop* worker : *workers) {
        worker->queueInLoop([worker, remaining_callbacks,
                             callbacks_ran_in_owner_thread, callbacks_finished] {
            if (!worker->isInLoopThread()) {
                callbacks_ran_in_owner_thread->store(false);
            }
            if (remaining_callbacks->fetch_sub(1) == 1) {
                callbacks_finished->set_value();
            }
        });
    }
    require(callback_result.wait_for(kOperationTimeout) == std::future_status::ready,
            "worker loops did not execute queued callbacks");
    callback_result.get();
    require(callbacks_ran_in_owner_thread->load(),
            "worker callback ran outside its EventLoop thread");

    auto stopped = std::make_shared<std::promise<void>>();
    auto stop_result = stopped->get_future();
    base_loop->queueInLoop([pool, stopped] {
        try {
            // 线程对象由 pool 独占；在基准 EventLoop 中销毁可保持其启动线程的调度边界。
            pool->reset();
            stopped->set_value();
        } catch (...) {
            stopped->set_exception(std::current_exception());
        }
    });
    require(stop_result.wait_for(kOperationTimeout) == std::future_status::ready,
            "timed out stopping legacy EventLoopThreadPool");
    stop_result.get();
}

void test_tcp_server_lifecycle_and_byte_stream_behavior() {
    ServerObservations observations;
    const std::uint16_t port = reserve_loopback_port();
    LegacyServerHarness server(port, observations);

    FileDescriptor first_client = connect_loopback(port);
    wait_for_observation(observations,
                         [](const ServerObservations& value) {
                             return value.connected_count == 1;
                         },
                         "server did not report the first connection");

    send_all(first_client.get(), "hel");
    send_all(first_client.get(), "lo");
    require(receive_exact(first_client.get(), 5) == "hello",
            "server did not preserve split TCP bytes");

    send_all(first_client.get(), "one");
    send_all(first_client.get(), "two");
    require(receive_exact(first_client.get(), 6) == "onetwo",
            "server did not preserve coalesced TCP bytes");

    FileDescriptor concurrent_client = connect_loopback(port);
    wait_for_observation(observations,
                         [](const ServerObservations& value) {
                             return value.connected_count == 2;
                         },
                         "server did not accept a concurrent connection");
    concurrent_client.shutdown_and_close();
    wait_for_observation(observations,
                         [](const ServerObservations& value) {
                             return value.disconnected_count == 1;
                         },
                         "server did not release the concurrent connection");

    first_client.shutdown_and_close();
    wait_for_observation(observations,
                         [](const ServerObservations& value) {
                             return value.disconnected_count == 2;
                         },
                         "server did not report peer-initiated disconnect");

    FileDescriptor force_close_client = connect_loopback(port);
    wait_for_observation(observations,
                         [](const ServerObservations& value) {
                             return value.connected_count == 3;
                         },
                         "server did not report the callback-close connection");
    send_all(force_close_client.get(), "force-close");
    wait_for_peer_close(force_close_client.get());
    wait_for_observation(observations,
                         [](const ServerObservations& value) {
                             return value.disconnected_count == 3;
                         },
                         "callback-triggered close did not release the connection");

    FileDescriptor live_client = connect_loopback(port);
    wait_for_observation(observations,
                         [](const ServerObservations& value) {
                             return value.connected_count == 4;
                         },
                         "server did not report the live connection before stop");
    server.stop();
    wait_for_peer_close(live_client.get());
    wait_for_observation(observations,
                         [](const ServerObservations& value) {
                             return value.disconnected_count == 4;
                         },
                         "server stop did not release the live connection");
}

}  // namespace

int main() {
    // glog 的时区初始化会触碰进程级状态；在线程启动前完成，避免干扰 Reactor 的 TSan 结果。
    google::InitGoogleLogging("legacy_network_characterization_test");
    try {
        test_buffer_preserves_partial_and_coalesced_bytes();
        test_event_loop_wakes_for_cross_thread_work();
        test_event_loop_thread_pool_starts_workers_and_rotates();
        test_tcp_server_lifecycle_and_byte_stream_behavior();
        std::cout << "legacy network characterization passed\n";
        google::ShutdownGoogleLogging();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "legacy network characterization failed: " << error.what()
                  << '\n';
        google::ShutdownGoogleLogging();
        return 1;
    }
}
