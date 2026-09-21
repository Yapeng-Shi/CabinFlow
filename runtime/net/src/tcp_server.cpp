#include <cabinflow/net/tcp_server.hpp>

#include <exception>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <cabinflow/net/event_loop.hpp>

#include "acceptor.hpp"
#include "event_loop_thread_pool.hpp"
#include "inet_address.hpp"
#include "socket.hpp"

namespace cabinflow::net {

struct TcpServer::Impl {
    Impl(EventLoop& loop_arg, std::string name_arg, std::string listen_ip,
         std::uint16_t listen_port)
        : loop(loop_arg),
          name(std::move(name_arg)),
          acceptor(std::make_unique<detail::Acceptor>(
              loop, detail::InetAddress(listen_ip, listen_port), false)),
          thread_pool(std::make_unique<detail::EventLoopThreadPool>(loop, 0)) {
        acceptor->set_new_connection_callback(
            [this](detail::Socket&& socket, const detail::InetAddress&) {
                handle_new_connection(std::move(socket));
            });
    }

    void handle_new_connection(detail::Socket&& socket) {
        const std::string connection_name = name + "-" + std::to_string(next_connection_id++);
        EventLoop* const io_loop = &thread_pool->next_loop();
        const int connection_file_descriptor = socket.release();
        if (io_loop == &loop) {
            create_connection_in_loop(*io_loop, connection_name, connection_file_descriptor);
            return;
        }
        io_loop->queue_in_loop([this, io_loop, connection_name, connection_file_descriptor] {
            create_connection_in_loop(*io_loop, connection_name, connection_file_descriptor);
        });
    }

    void create_connection_in_loop(EventLoop& io_loop, const std::string& connection_name,
                                   int connection_file_descriptor) {
        detail::Socket unowned_connection(connection_file_descriptor);
        {
            std::lock_guard<std::mutex> lock(connection_mutex);
            if (!accepting) {
                return;
            }
        }

        const TcpConnectionPtr connection = TcpConnection::create(
            io_loop, connection_name, unowned_connection.release());
        connection->set_connection_callback(connection_callback);
        connection->set_message_callback(message_callback);
        connection->set_close_callback([this](const TcpConnectionPtr& value) {
            remove_connection(value);
        });
        {
            std::lock_guard<std::mutex> lock(connection_mutex);
            if (!accepting) {
                return;
            }
            connections.emplace(connection_name, connection);
        }
        connection->establish();
    }

    void remove_connection(const TcpConnectionPtr& value) {
        {
            std::lock_guard<std::mutex> lock(connection_mutex);
            const auto iterator = connections.find(value->name());
            if (iterator != connections.end() && iterator->second == value) {
                connections.erase(iterator);
            }
        }
        connections_changed.notify_all();
    }

    void assert_in_loop_thread() const {
        if (!loop.is_in_loop_thread()) {
            throw std::logic_error("TcpServer accessed outside EventLoop owner thread");
        }
    }

    EventLoop& loop;
    std::string name;
    std::unique_ptr<detail::Acceptor> acceptor;
    std::unique_ptr<detail::EventLoopThreadPool> thread_pool;
    ConnectionCallback connection_callback;
    MessageCallback message_callback;
    std::map<std::string, TcpConnectionPtr> connections;
    std::mutex connection_mutex;
    std::condition_variable connections_changed;
    std::size_t next_connection_id{1};
    std::size_t worker_count{0};
    bool started{false};
    bool accepting{false};
};

TcpServer::TcpServer(EventLoop& loop, std::string name, std::string listen_ip,
                     std::uint16_t listen_port) {
    if (!loop.is_in_loop_thread()) {
        throw std::logic_error("TcpServer must be created in EventLoop owner thread");
    }
    impl_ = std::make_unique<Impl>(loop, std::move(name), std::move(listen_ip), listen_port);
}

TcpServer::~TcpServer() { stop(); }

void TcpServer::set_connection_callback(ConnectionCallback callback) {
    impl_->assert_in_loop_thread();
    if (impl_->started) {
        throw std::logic_error("TcpServer callbacks must be set before start");
    }
    impl_->connection_callback = std::move(callback);
}

void TcpServer::set_message_callback(MessageCallback callback) {
    impl_->assert_in_loop_thread();
    if (impl_->started) {
        throw std::logic_error("TcpServer callbacks must be set before start");
    }
    impl_->message_callback = std::move(callback);
}

void TcpServer::set_worker_count(std::size_t worker_count) {
    impl_->assert_in_loop_thread();
    if (impl_->started) {
        throw std::logic_error("TcpServer worker count must be set before start");
    }
    impl_->worker_count = worker_count;
    impl_->thread_pool =
        std::make_unique<detail::EventLoopThreadPool>(impl_->loop, worker_count);
}

void TcpServer::start() {
    impl_->assert_in_loop_thread();
    if (impl_->started) {
        throw std::logic_error("TcpServer is already started");
    }
    impl_->thread_pool->start();
    {
        std::lock_guard<std::mutex> lock(impl_->connection_mutex);
        impl_->accepting = true;
    }
    impl_->started = true;
    impl_->acceptor->listen();
}

void TcpServer::stop() {
    impl_->assert_in_loop_thread();
    if (!impl_->started) {
        return;
    }

    impl_->started = false;
    impl_->acceptor->stop();
    std::vector<TcpConnectionPtr> connections;
    {
        std::lock_guard<std::mutex> lock(impl_->connection_mutex);
        impl_->accepting = false;
        connections.reserve(impl_->connections.size());
        for (const auto& entry : impl_->connections) {
            connections.push_back(entry.second);
        }
    }

    // 复制 map 中的强引用后再逐个关闭；worker 的 close 回调会异步擦除原 map。
    for (const TcpConnectionPtr& connection : connections) {
        connection->force_close();
    }
    {
        std::unique_lock<std::mutex> lock(impl_->connection_mutex);
        impl_->connections_changed.wait(lock, [this] { return impl_->connections.empty(); });
    }
    impl_->thread_pool->stop();
}

std::uint16_t TcpServer::bound_port() const {
    impl_->assert_in_loop_thread();
    return impl_->acceptor->bound_port();
}

std::size_t TcpServer::connection_count() const {
    impl_->assert_in_loop_thread();
    std::lock_guard<std::mutex> lock(impl_->connection_mutex);
    return impl_->connections.size();
}

}  // namespace cabinflow::net
