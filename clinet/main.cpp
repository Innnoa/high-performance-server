#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

// 安全的字符串到整数转换函数
namespace utils
{
    int safe_string_to_int(const char* str, const std::string& context = "")
    {
        if (!str || *str == '\0')
        {
            throw std::invalid_argument("Empty string cannot be converted to integer" +
                (context.empty() ? "" : " (" + context + ")"));
        }

        char* endptr = nullptr;
        errno = 0;

        const long result = std::strtol(str, &endptr, 10);

        // 检查转换错误
        if (errno == ERANGE)
        {
            throw std::out_of_range("Number out of range: " + std::string(str) +
                (context.empty() ? "" : " (" + context + ")"));
        }

        if (endptr == str)
        {
            throw std::invalid_argument("No valid conversion could be performed: " + std::string(str) +
                (context.empty() ? "" : " (" + context + ")"));
        }

        if (*endptr != '\0')
        {
            throw std::invalid_argument("Invalid characters found in number: " + std::string(str) +
                (context.empty() ? "" : " (" + context + ")"));
        }

        // 检查是否在int范围内
        if (result < INT_MIN || result > INT_MAX)
        {
            throw std::out_of_range("Number out of int range: " + std::string(str) +
                (context.empty() ? "" : " (" + context + ")"));
        }

        return static_cast<int>(result);
    }
}

class epoll_client
{
public:
    static constexpr size_t buffer_size = 128;
    static constexpr size_t max_events = 1024;
    static constexpr size_t max_connections = 700000;
    static constexpr size_t connections_per_batch = 1000;
    static constexpr int max_ports = 20;

private:
    struct connection
    {
        int fd;
        std::string write_buffer;
        std::string read_buffer;
        bool connected = false;
        bool data_sent = false;
        std::chrono::steady_clock::time_point connect_time;

        explicit connection(const int fd) : fd(fd), connect_time(std::chrono::steady_clock::now())
        {
        }
    };

    // RAII wrapper for file descriptors
    class file_descriptor
    {
        int fd_;

    public:
        explicit file_descriptor(const int fd) : fd_(fd)
        {
        }

        ~file_descriptor()
        {
            if (fd_ >= 0)
            {
                close(fd_);
            }
        }

        file_descriptor(const file_descriptor&) = delete;
        file_descriptor& operator=(const file_descriptor&) = delete;

        file_descriptor(file_descriptor&& other) noexcept : fd_(other.fd_)
        {
            other.fd_ = -1;
        }

        file_descriptor& operator=(file_descriptor&& other) noexcept
        {
            if (this != &other)
            {
                if (fd_ >= 0)
                {
                    close(fd_);
                }
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }

        [[nodiscard]] int get() const { return fd_; }
        explicit operator int() const { return fd_; }

        int release()
        {
            const int temp = fd_;
            fd_ = -1;
            return temp;
        }
    };

    file_descriptor epfd_;
    std::unordered_map<int, std::unique_ptr<connection>> connections_;
    std::string server_ip_;
    int base_port_;
    int current_port_index_;
    size_t total_connections_;
    std::atomic<bool> running_{true};
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_report_time_;

    // 连接失败统计
    size_t connection_failures_ = 0;
    std::chrono::steady_clock::time_point last_failure_time_;
    static constexpr size_t max_consecutive_failures = 100;

public:
    epoll_client(std::string ip, const int port)
        : epfd_(epoll_create1(EPOLL_CLOEXEC))
          , server_ip_(std::move(ip))
          , base_port_(port)
          , current_port_index_(0)
          , total_connections_(0)
    {
        if (epfd_.get() < 0)
        {
            throw std::runtime_error("Failed to create epoll");
        }

        start_time_ = std::chrono::steady_clock::now();
        last_report_time_ = start_time_;
        last_failure_time_ = start_time_;
    }

    void run()
    {
        std::array<epoll_event, max_events> events{};

        std::cout << "Starting client, connecting to " << server_ip_
            << " ports " << base_port_ << "-" << (base_port_ + max_ports - 1)
            << std::endl;

        size_t no_progress_count = 0;
        size_t last_total_connections = 0;
        auto last_check_time = std::chrono::steady_clock::now();

        while (running_ && total_connections_ < max_connections)
        {
            // 创建新连接
            create_connections();

            // 处理事件
            const int nfds = epoll_wait(epfd_.get(), events.data(), max_events, 10);

            if (nfds < 0)
            {
                if (errno == EINTR) continue;
                std::cerr << "epoll_wait failed: " << strerror(errno) << std::endl;
                break;
            }

            for (int i = 0; i < nfds; ++i)
            {
                handle_event(events[i]);
            }

            // 定期报告统计信息
            report_stats_if_needed();

            // 检查是否服务器完全不可用
            auto now = std::chrono::steady_clock::now();
            const auto time_diff = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_check_time).count();

            if (time_diff >= 10) // 每10秒检查一次
            {
                if (total_connections_ == last_total_connections &&
                    connections_.empty())
                {
                    no_progress_count++;
                    if (no_progress_count >= 3) // 30秒没有进展
                    {
                        std::cout << "No progress for 30 seconds, server may be down. Stopping..." << std::endl;
                        break;
                    }
                    std::cout << "Warning: No active connections and no progress. "
                        << "Server may be overloaded or down." << std::endl;
                }
                else
                {
                    no_progress_count = 0;
                }

                last_total_connections = total_connections_;
                last_check_time = now;
            }

            // 短暂休眠，避免CPU占用过高
            usleep(500);
        }

        // 最终统计
        final_report();
    }

    void stop()
    {
        running_ = false;
    }

private:
    void create_connections()
    {
        static size_t batch_count = 0;

        // 检查是否需要暂停连接（连续失败太多次）
        if (connection_failures_ >= max_consecutive_failures)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto time_since_failure = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_failure_time_).count();

            if (time_since_failure < 5) // 暂停5秒
            {
                return;
            }

            // 重置失败计数，重新尝试
            connection_failures_ = 0;
            std::cout << "Resuming connection attempts after pause..." << std::endl;
        }

        // 每批创建一定数量的连接
        if (batch_count >= connections_per_batch)
        {
            batch_count = 0;
            return;
        }

        if (total_connections_ >= max_connections) return;

        try
        {
            int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (sockfd < 0)
            {
                record_connection_failure("Failed to create socket: " + std::string(strerror(errno)));
                return;
            }

            // 设置socket选项
            set_socket_options(sockfd);

            // 准备服务器地址
            sockaddr_in server_addr{};
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(base_port_ + current_port_index_);

            if (inet_pton(AF_INET, server_ip_.c_str(), &server_addr.sin_addr) <= 0)
            {
                close(sockfd);
                record_connection_failure("Invalid address: " + server_ip_);
                return;
            }

            // 尝试连接
            const int result = connect(sockfd, reinterpret_cast<sockaddr*>(&server_addr),
                                       sizeof(server_addr));

            if (result < 0 && errno != EINPROGRESS)
            {
                close(sockfd);
                // 只在非预期错误时记录失败
                if (errno != ECONNREFUSED && errno != ECONNRESET)
                {
                    record_connection_failure("Connect failed: " + std::string(strerror(errno)));
                }
                else
                {
                    // 连接被拒绝是正常的，服务器可能关闭了
                    connection_failures_++;
                    last_failure_time_ = std::chrono::steady_clock::now();
                }
                return;
            }

            // 连接成功或正在进行中，添加到epoll
            add_to_epoll(sockfd, EPOLLOUT | EPOLLIN | EPOLLET);

            // 创建连接对象
            auto conn = std::make_unique<connection>(sockfd);
            conn->write_buffer = "Hello Server: client --> " + std::to_string(total_connections_) + "\n";
            connections_[sockfd] = std::move(conn);

            total_connections_++;
            batch_count++;

            // 重置失败计数（成功创建连接）
            connection_failures_ = 0;

            // 轮询端口
            current_port_index_ = (current_port_index_ + 1) % max_ports;
        }
        catch (const std::exception& e)
        {
            record_connection_failure("Exception in create_connections: " + std::string(e.what()));
        }
    }

    void record_connection_failure(const std::string& error_msg)
    {
        connection_failures_++;
        last_failure_time_ = std::chrono::steady_clock::now();

        // 只在失败次数较少时打印错误信息，避免日志泛滥
        if (connection_failures_ <= 10 || connection_failures_ % 50 == 0)
        {
            std::cerr << "Connection failure #" << connection_failures_ << ": " << error_msg << std::endl;
        }

        if (connection_failures_ == max_consecutive_failures)
        {
            std::cout << "Too many consecutive failures (" << max_consecutive_failures
                << "), pausing connection attempts..." << std::endl;
        }
    }

    static void set_socket_options(const int sockfd)
    {
        // 设置TCP_NODELAY
        constexpr int nodelay = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // 设置SO_REUSEADDR
        constexpr int reuse = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }

    void add_to_epoll(const int fd, uint32_t events) const
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;

        if (epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0)
        {
            std::cerr << "Failed to add fd to epoll: " << strerror(errno) << std::endl;
        }
    }

    void modify_epoll(const int fd, uint32_t events) const
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;

        if (epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0)
        {
            std::cerr << "Failed to modify epoll: " << strerror(errno) << std::endl;
        }
    }

    void remove_from_epoll(const int fd) const
    {
        epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
    }

    void handle_event(const epoll_event& event)
    {
        const int fd = event.data.fd;
        auto it = connections_.find(fd);

        if (it == connections_.end())
        {
            // fd已经被清理，忽略这个事件
            return;
        }

        const auto& conn = it->second;

        // 处理错误和挂起事件
        if (event.events & (EPOLLERR | EPOLLHUP))
        {
            // 区分错误类型，避免过多日志
            if (event.events & EPOLLERR)
            {
                handle_error(fd, "Connection error", false);
            }
            else
            {
                // EPOLLHUP通常是正常的连接关闭，不打印错误日志
                close_connection(fd);
            }
            return;
        }

        // 处理写事件
        if (event.events & EPOLLOUT)
        {
            if (!handle_write(fd, conn.get()))
            {
                return; // 连接已关闭
            }
        }

        // 重新检查连接是否还存在（可能在write中被关闭）
        it = connections_.find(fd);
        if (it == connections_.end())
        {
            return;
        }

        // 处理读事件
        if (event.events & EPOLLIN)
        {
            handle_read(fd, it->second.get());
        }
    }

    bool handle_write(const int fd, connection* conn)
    {
        if (conn->data_sent || conn->write_buffer.empty())
        {
            return true; // 没有数据需要发送，连接正常
        }

        const ssize_t sent = send(fd, conn->write_buffer.c_str(),
                                  conn->write_buffer.length(), MSG_NOSIGNAL);

        if (sent > 0)
        {
            if (static_cast<size_t>(sent) >= conn->write_buffer.length())
            {
                conn->data_sent = true;
                conn->write_buffer.clear();
                // 只监听读事件
                modify_epoll(fd, EPOLLIN | EPOLLET);
            }
            else
            {
                // 部分发送，移除已发送的部分
                conn->write_buffer.erase(0, sent);
            }
            return true;
        }
        if (sent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return true; // 暂时无法发送，但连接正常
            }
            handle_error(fd, "Write error: " + std::string(strerror(errno)), true);
            return false; // 连接已关闭
        }

        return true;
    }

    void handle_read(const int fd, connection* conn)
    {
        char buffer[buffer_size];

        if (const ssize_t received = recv(fd, buffer, sizeof(buffer) - 1, 0); received > 0)
        {
            buffer[received] = '\0';
            conn->read_buffer += buffer;

            // 检查是否收到quit命令
            if (conn->read_buffer.find("quit") != std::string::npos)
            {
                std::cout << "Received quit command, stopping..." << std::endl;
                running_ = false;
            }
        }
        else if (received == 0)
        {
            // 连接正常关闭，不打印错误日志
            close_connection(fd);
        }
        else
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                handle_error(fd, "Read error: " + std::string(strerror(errno)), true);
            }
        }
    }

    void handle_error(const int fd, const std::string& error_msg, const bool print_error = true)
    {
        if (print_error)
        {
            std::cerr << "Error on fd " << fd << ": " << error_msg << std::endl;
        }
        close_connection(fd);
    }

    void close_connection(const int fd)
    {
        const auto it = connections_.find(fd);
        if (it == connections_.end())
        {
            // 连接已经被关闭，避免重复处理
            return;
        }

        remove_from_epoll(fd);
        connections_.erase(it);
        close(fd);

        // 减少活跃连接计数
        if (total_connections_ > connections_.size())
        {
            // 这里可以添加重连逻辑或统计信息
        }
    }

    void report_stats_if_needed()
    {
        static size_t last_reported_connections = 0;
        static size_t last_active_connections = 0;
        static size_t stable_active_count = 0;
        static auto last_active_change_time = std::chrono::steady_clock::now();
        static auto last_forced_report_time = std::chrono::steady_clock::now();
        static bool currently_reporting = false;  // 防止重入

        // 防止在同一个事件循环中重复调用
        if (currently_reporting) {
            return;
        }
        currently_reporting = true;

        const auto now = std::chrono::steady_clock::now();
        const size_t current_active = connections_.size();

        // 判断是否需要打印统计信息
        bool should_report = false;
        std::string report_reason;

        // 条件1：每1000个连接打印一次（确保只打印一次）
        if (total_connections_ / 1000 > last_reported_connections / 1000)
        {
            should_report = true;
            report_reason = "milestone";
        }

        // 条件2：每30秒强制打印一次（避免长时间无输出）
        const auto time_since_last_forced = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_forced_report_time).count();
        if (time_since_last_forced >= 30 && !should_report)
        {
            should_report = true;
            report_reason = "periodic";
            last_forced_report_time = now;
        }

        // 条件3：活跃连接数发生显著变化（超过5000个连接的变化或下降超过1000）
        const int active_diff = static_cast<int>(current_active) - static_cast<int>(last_active_connections);
        if (abs(active_diff) > 5000 || (active_diff < -1000 && !should_report))
        {
            should_report = true;
            report_reason = "active_change";
        }

        // 如果不需要报告，更新状态后返回
        if (!should_report)
        {
            // 但仍需要检测稳定状态
            if (current_active == last_active_connections)
            {
                stable_active_count++;
            }
            else
            {
                stable_active_count = 0;
                last_active_change_time = now;
            }
            last_active_connections = current_active;
            currently_reporting = false;
            return;
        }

        const auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time_).count();

        if (total_duration == 0)
        {
            currently_reporting = false;
            return;
        }

        const double rate =static_cast<double>(total_connections_) * 1000.0 / static_cast<double>(total_duration);

        // 检测活跃连接数是否停止增长
        if (current_active == last_active_connections)
        {
            stable_active_count++;
        }
        else
        {
            stable_active_count = 0;
            last_active_change_time = now;
        }

        // 计算成功率
        const double success_rate = total_connections_ > 0 ?
            static_cast<double>(current_active)* 100.0 / static_cast<double>(total_connections_) : 0;

        std::cout
            << "Created: " << total_connections_
            << ", Active: " << current_active
            << ", Success: " << std::fixed << std::setprecision(1) << success_rate << "%"
            << ", Time: " << total_duration << "ms"
            << ", Rate: " << std::fixed << std::setprecision(2) << rate << " conn/s";

        // 添加报告原因和异常标识
        if (report_reason == "milestone")
        {
            std::cout << " (milestone)";
        }
        else if (report_reason == "periodic")
        {
            std::cout << " (periodic)";
        }
        else if (report_reason == "active_change")
        {
            std::cout << " (active_change)";
            // 如果是因为连接数下降触发的报告，添加警告
            if (active_diff < -1000)
            {
                std::cout << "  DROPPING";
            }
        }

        // 检测连接成功率异常
        if (success_rate < 90.0 && total_connections_ > 10000)
        {
            std::cout << " LOW_SUCCESS";
        }

        std::cout << std::endl;

        // 检测可能的服务器限制
        if (stable_active_count >= 3 && total_connections_ > current_active + 1000)
        {
            const auto time_since_change = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_active_change_time).count();

            if (time_since_change >= 30)
            {
                std::cout
                    << " WARNING: Active connections stable at " << current_active
                    << " for " << time_since_change << " seconds." << std::endl;
                std::cout
                    << "   Server may have reached capacity. Failed: "
                    << (total_connections_ - current_active) << std::endl;

                // 建议停止测试
                if (time_since_change >= 60)
                {
                    std::cout
                        << " Stopping - server capacity reached." << std::endl;
                    running_ = false;
                    currently_reporting = false;
                    return;
                }
            }
        }

        // 更新状态
        last_reported_connections = total_connections_;
        last_active_connections = current_active;
        currently_reporting = false;
    }

    void final_report()
    {
        const auto end_time = std::chrono::steady_clock::now();
        const auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time_).count();

        const double rate = static_cast<double>(total_connections_) * 1000.0 / static_cast<double>(total_duration);

        const size_t disconnected = total_connections_ - connections_.size();

        std::cout << "\n=== Final Statistics ===" << std::endl;
        std::cout << "Total connections created: " << total_connections_ << std::endl;
        std::cout << "Active connections: " << connections_.size() << std::endl;
        std::cout << "Disconnected connections: " << disconnected << std::endl;
        std::cout << "Connection success rate: " << std::fixed << std::setprecision(1)
            << (total_connections_ > 0
                    ? static_cast<double>(connections_.size()) * 100.0 / static_cast<double>(total_connections_)
                    : 0)
            << "%" << std::endl;
        std::cout << "Total time: " << total_duration << "ms" << std::endl;
        std::cout << "Average creation rate: " << std::fixed << std::setprecision(2)
            << rate << " conn/s" << std::endl;
        std::cout << "Connection failures: " << connection_failures_ << std::endl;

        // 清理剩余连接
        if (!connections_.empty())
        {
            std::cout << "Cleaning up " << connections_.size() << " remaining connections..." << std::endl;
            connections_.clear();
        }
    }
};

int main(const int argc, char** argv)
{
    if (argc != 3)
    {
        std::cout << "Usage: " << argv[0] << " <server_ip> <base_port>" << std::endl;
        std::cout << "Example: " << argv[0] << " 127.0.0.1 2048" << std::endl;
        return 1;
    }

    try
    {
        const std::string server_ip = argv[1];
        const int base_port = utils::safe_string_to_int(argv[2], "port number");

        if (base_port <= 0 || base_port > 65535)
        {
            std::cerr << "Invalid port number: " << base_port
                << " (must be between 1 and 65535)" << std::endl;
            return 1;
        }

        epoll_client client(server_ip, base_port);

        // 设置信号处理
        signal(SIGINT, [](int)
        {
            std::cout << "\nReceived interrupt signal, shutting down..." << std::endl;
            exit(0);
        });

        signal(SIGPIPE, SIG_IGN);

        client.run();
    }
    catch (const std::invalid_argument& e)
    {
        std::cerr << "Invalid argument: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::out_of_range& e)
    {
        std::cerr << "Out of range: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
