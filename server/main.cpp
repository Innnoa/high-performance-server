#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <queue>
#include <csignal>
#include <sstream>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

class leak_safe_high_perf_server
{
public:
    static constexpr size_t small_buffer_size = 256;
    static constexpr size_t max_events = 8192;
    static constexpr int backlog = 1024;
    static constexpr size_t max_connections = 2100000;
    static constexpr size_t pool_chunk_size = 10000;
    static constexpr size_t cleanup_batch_size = 1000;

private:
    struct connection
    {
        int fd;
        uint32_t last_activity;
        uint16_t read_pos;
        uint16_t write_pos;
        char* read_buffer;
        char* write_buffer;

        connection() : fd(-1), last_activity(0), read_pos(0), write_pos(0),
                       read_buffer(nullptr), write_buffer(nullptr)
        {
        }
    };

    // 使用 RAII 智能指针管理内存块
    class safe_memory_pool
    {
        struct buffer_chunk
        {
            std::unique_ptr<char[]> data;
            size_t size;

            buffer_chunk(const size_t count, const size_t buffer_size)
                : data(std::make_unique<char[]>(count * buffer_size))
                  , size(count * buffer_size)
            {
            }
        };

        std::vector<buffer_chunk> buffer_chunks_;
        std::queue<char*> free_buffers_;
        size_t chunk_size_;
        size_t buffer_size_;

    public:
        safe_memory_pool(const size_t buffer_size, const size_t initial_count)
            : chunk_size_(initial_count), buffer_size_(buffer_size)
        {
            allocate_chunk();
        }

        // 析构函数自动管理，不需要手动delete
        ~safe_memory_pool() = default;

        // 禁用拷贝，只允许移动
        safe_memory_pool(const safe_memory_pool&) = delete;
        safe_memory_pool& operator=(const safe_memory_pool&) = delete;
        safe_memory_pool(safe_memory_pool&&) noexcept = default;
        safe_memory_pool& operator=(safe_memory_pool&&) = default;

        char* get_buffer()
        {
            if (free_buffers_.empty())
            {
                allocate_chunk();
            }

            if (free_buffers_.empty()) return nullptr;

            char* buffer = free_buffers_.front();
            free_buffers_.pop();
            return buffer;
        }

        void return_buffer(char* buffer)
        {
            if (buffer && is_valid_buffer(buffer))
            {
                free_buffers_.push(buffer);
            }
        }

        [[nodiscard]] size_t get_total_allocated() const
        {
            return buffer_chunks_.size() * chunk_size_ * buffer_size_;
        }

    private:
        void allocate_chunk()
        {
            try
            {
                // 使用RAII管理内存块
                buffer_chunks_.emplace_back(chunk_size_, buffer_size_);
                const auto& chunk = buffer_chunks_.back();

                // 将缓冲区添加到空闲列表
                char* base = chunk.data.get();
                for (size_t i = 0; i < chunk_size_; ++i)
                {
                    free_buffers_.push(base + i * buffer_size_);
                }
            }
            catch (const std::bad_alloc&)
            {
                std::cerr << "Failed to allocate memory chunk" << std::endl;
                // 不抛出异常，让调用者处理nullptr
            }
        }

        bool is_valid_buffer(const char* buffer) const
        {
            // 验证缓冲区是否属于我们的内存池
            for (const auto& chunk : buffer_chunks_)
            {
                const char* start = chunk.data.get();
                if (const char* end = start + chunk.size; buffer >= start && buffer < end)
                {
                    // 检查对齐
                    const size_t offset = buffer - start;
                    return offset % buffer_size_ == 0;
                }
            }
            return false;
        }
    };

    // 使用 RAII 管理连接池
    class safe_connection_pool
    {
        struct connection_chunk
        {
            std::unique_ptr<connection[]> data;
            size_t count;

            explicit connection_chunk(const size_t chunk_count)
                : data(std::make_unique<connection[]>(chunk_count))
                  , count(chunk_count)
            {
            }
        };

        std::vector<connection_chunk> connection_chunks_;
        std::queue<connection*> free_connections_;
        size_t chunk_size_;

    public:
        explicit safe_connection_pool(const size_t initial_count) : chunk_size_(initial_count)
        {
            allocate_chunk();
        }

        // 析构函数自动管理，不需要手动delete
        ~safe_connection_pool() = default;

        // 禁用拷贝，只允许移动
        safe_connection_pool(const safe_connection_pool&) = delete;
        safe_connection_pool& operator=(const safe_connection_pool&) = delete;
        safe_connection_pool(safe_connection_pool&&) noexcept = default;
        safe_connection_pool& operator=(safe_connection_pool&&) = default;

        connection* get_connection()
        {
            if (free_connections_.empty())
            {
                allocate_chunk();
            }

            if (free_connections_.empty()) return nullptr;

            connection* conn = free_connections_.front();
            free_connections_.pop();
            return conn;
        }

        void return_connection(connection* conn)
        {
            if (conn && is_valid_connection(conn))
            {
                // 重置连接状态
                conn->fd = -1;
                conn->last_activity = 0;
                conn->read_pos = 0;
                conn->write_pos = 0;
                conn->read_buffer = nullptr;
                conn->write_buffer = nullptr;

                free_connections_.push(conn);
            }
        }

        [[nodiscard]] size_t get_total_allocated() const
        {
            return connection_chunks_.size() * chunk_size_;
        }

    private:
        void allocate_chunk()
        {
            try
            {
                // 使用RAII管理连接块
                connection_chunks_.emplace_back(chunk_size_);
                const auto& chunk = connection_chunks_.back();

                // 将连接添加到空闲列表
                connection* base = chunk.data.get();
                for (size_t i = 0; i < chunk_size_; ++i)
                {
                    free_connections_.push(&base[i]);
                }
            }
            catch (const std::bad_alloc&)
            {
                std::cerr << "Failed to allocate connection chunk" << std::endl;
                // 不抛出异常，让调用者处理nullptr
            }
        }

        bool is_valid_connection(const connection* conn) const
        {
            // 验证连接是否属于我们的连接池
            for (const auto& chunk : connection_chunks_)
            {
                const connection* start = chunk.data.get();
                if (const connection* end = start + chunk.count; conn >= start && conn < end)
                {
                    return true;
                }
            }
            return false;
        }
    };

    // RAII文件描述符包装
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
                if (fd_ >= 0) close(fd_);
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }

        [[nodiscard]] int get() const { return fd_; }
        explicit operator int() const { return fd_; }
        [[nodiscard]] bool valid() const { return fd_ >= 0; }
    };

    file_descriptor epfd_;
    std::vector<connection*> connections_;
    std::vector<std::unique_ptr<file_descriptor>> listen_sockets_;

    // 使用安全的内存池
    std::unique_ptr<safe_memory_pool> read_buffer_pool_;
    std::unique_ptr<safe_memory_pool> write_buffer_pool_;
    std::unique_ptr<safe_connection_pool> connection_pool_;

    std::atomic<bool> running_{true};
    std::atomic<uint64_t> connection_count_{0};
    std::atomic<uint64_t> total_bytes_read_{0};
    std::atomic<uint64_t> total_bytes_written_{0};

    timeval start_time_{};

    struct statistics
    {
        std::atomic<uint64_t> accepts{0};
        std::atomic<uint64_t> reads{0};
        std::atomic<uint64_t> writes{0};
        std::atomic<uint64_t> closes{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint64_t> memory_alloc_fails{0};
    } stats_;

public:
    leak_safe_high_perf_server() : epfd_(epoll_create1(EPOLL_CLOEXEC))
    {
        if (epfd_.get() < 0)
        {
            throw std::runtime_error("Failed to create epoll");
        }

        // 初始化安全内存池
        try
        {
            read_buffer_pool_ = std::make_unique<safe_memory_pool>(small_buffer_size, pool_chunk_size);
            write_buffer_pool_ = std::make_unique<safe_memory_pool>(small_buffer_size, pool_chunk_size);
            connection_pool_ = std::make_unique<safe_connection_pool>(pool_chunk_size);
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("Failed to initialize memory pools: " + std::string(e.what()));
        }

        connections_.resize(max_connections + 1000, nullptr);

        signal(SIGPIPE, SIG_IGN);
        gettimeofday(&start_time_, nullptr);

        optimize_system_settings();

        std::cout << " Leak-safe high-performance server initialized" << std::endl;
        print_memory_info();
    }

    // 析构函数确保所有资源正确释放
    ~leak_safe_high_perf_server()
    {
        std::cout << " Server shutdown - cleaning up resources..." << std::endl;

        // 关闭所有连接
        cleanup_all_connections();

        // 智能指针会自动管理内存池
        std::cout << " All resources cleaned up successfully" << std::endl;
    }

    void add_listen_port(const uint16_t port)
    {
        int sockfd_raw = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (sockfd_raw < 0)
        {
            throw std::runtime_error("Failed to create socket");
        }

        auto sockfd = std::make_unique<file_descriptor>(sockfd_raw);

        configure_socket_options(sockfd->get());
        bind_and_listen(sockfd->get(), port);
        add_to_epoll(sockfd->get(), EPOLLIN | EPOLLET);

        listen_sockets_.push_back(std::move(sockfd));
        std::cout << "Listening on port " << port << std::endl;
    }

    void run()
    {
        std::array<epoll_event, max_events> events{};
        auto last_cleanup = std::chrono::steady_clock::now();
        auto last_stats = std::chrono::steady_clock::now();
        auto last_memory_check = std::chrono::steady_clock::now();

        std::cout << " Leak-safe server started (target: 1M connections)" << std::endl;
        print_optimizations();

        while (running_)
        {
            const int nfds = epoll_wait(epfd_.get(), events.data(), max_events, 100);

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

            auto now = std::chrono::steady_clock::now();

            // 定期清理
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup).count() >= 30)
            {
                cleanup_idle_connections();
                last_cleanup = now;
            }

            // 定期统计
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count() >= 10)
            {
                print_performance_stats();
                last_stats = now;
            }

            // 定期内存检查
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_memory_check).count() >= 60)
            {
                check_memory_health();
                last_memory_check = now;
            }
        }
    }

    void stop() { running_ = false; }

private:
    static void configure_socket_options(const int fd)
    {
        constexpr int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        constexpr int buf_size = 65536;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    }

    static void bind_and_listen(const int fd, const uint16_t port)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            throw std::runtime_error("Failed to bind socket");
        }

        if (listen(fd, backlog) < 0)
        {
            throw std::runtime_error("Failed to listen on socket");
        }
    }

    void add_to_epoll(const int fd, uint32_t events) const
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;

        if (epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0)
        {
            throw std::runtime_error("Failed to add fd to epoll");
        }
    }

    void modify_epoll(const int fd, const uint32_t events) const
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev);
    }

    void remove_from_epoll(const int fd) const
    {
        epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
    }

    [[nodiscard]] bool is_listen_socket(const int fd) const
    {
        for (const auto& sock : listen_sockets_)
        {
            if (sock->get() == fd) return true;
        }
        return false;
    }

    void handle_event(const epoll_event& event)
    {
        const int fd = event.data.fd;

        if (event.events & (EPOLLERR | EPOLLHUP))
        {
            if (!is_listen_socket(fd))
            {
                close_connection(fd);
                ++stats_.errors;
            }
            return;
        }

        if (is_listen_socket(fd))
        {
            if (event.events & EPOLLIN)
            {
                accept_connections(fd);
            }
        }
        else
        {
            if (event.events & EPOLLIN)
            {
                handle_read(fd);
            }
            if (event.events & EPOLLOUT)
            {
                handle_write(fd);
            }
        }
    }

    void accept_connections(const int listen_fd)
    {
        int accepted = 0;
        constexpr int max_accept_batch = 100;

        while (accepted < max_accept_batch)
        {
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);

            const int client_fd = accept4(listen_fd, reinterpret_cast<sockaddr*>(&client_addr),
                                          &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (client_fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;

                if (errno == EMFILE || errno == ENFILE)
                {
                    std::cerr << " File descriptor limit reached!" << std::endl;
                    cleanup_idle_connections();
                    break;
                }

                ++stats_.errors;
                break;
            }

            if (!setup_new_connection(client_fd))
            {
                close(client_fd);
                break;
            }

            accepted++;
        }
    }

    bool setup_new_connection(const int fd)
    {
        // 检查连接数限制
        if (connection_count_ >= max_connections)
        {
            return false;
        }

        if (static_cast<size_t>(fd) >= connections_.size())
        {
            return false;
        }

        // 从连接池获取连接对象
        connection* conn = connection_pool_->get_connection();
        if (!conn)
        {
            ++stats_.memory_alloc_fails;
            return false;
        }

        // 分配读写缓冲区
        conn->read_buffer = read_buffer_pool_->get_buffer();
        conn->write_buffer = write_buffer_pool_->get_buffer();

        if (!conn->read_buffer || !conn->write_buffer)
        {
            // 清理已分配的资源
            if (conn->read_buffer) read_buffer_pool_->return_buffer(conn->read_buffer);
            if (conn->write_buffer) write_buffer_pool_->return_buffer(conn->write_buffer);
            connection_pool_->return_connection(conn);
            ++stats_.memory_alloc_fails;
            return false;
        }

        // 初始化连接
        conn->fd = fd;
        conn->last_activity = get_current_time();
        conn->read_pos = 0;
        conn->write_pos = 0;

        // 设置TCP选项
        constexpr int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // 添加到epoll
        if (add_to_epoll_safe(fd, EPOLLIN | EPOLLET) != 0)
        {
            cleanup_connection_resources(conn);
            return false;
        }

        connections_[fd] = conn;
        ++connection_count_;
        ++stats_.accepts;

        // 每1000个连接打印统计
        if (connection_count_ % 1000 == 0)
        {
            print_stats();
        }

        return true;
    }

    [[nodiscard]] int add_to_epoll_safe(const int fd, uint32_t events) const
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev);
    }

    void handle_read(const int fd)
    {
        if (static_cast<size_t>(fd) >= connections_.size() || !connections_[fd])
        {
            return;
        }

        connection* conn = connections_[fd];
        conn->last_activity = get_current_time();

        ssize_t total_read = 0;
        constexpr int max_read_loops = 16;

        for (int i = 0; i < max_read_loops; ++i)
        {
            size_t available = small_buffer_size - conn->read_pos;
            if (available == 0)
            {
                process_request_fast(conn);
                available = small_buffer_size - conn->read_pos;
                if (available == 0) break;
            }

            const ssize_t n = recv(fd, conn->read_buffer + conn->read_pos, available, 0);

            if (n > 0)
            {
                conn->read_pos += n;
                total_read += n;
            }
            else if (n == 0)
            {
                close_connection(fd);
                return;
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close_connection(fd);
                ++stats_.errors;
                return;
            }
        }

        if (total_read > 0)
        {
            total_bytes_read_ += total_read;
            ++stats_.reads;

            process_request_fast(conn);

            if (conn->write_pos > 0)
            {
                modify_epoll(fd, EPOLLOUT | EPOLLET);
            }
        }
    }

    void handle_write(const int fd)
    {
        if (static_cast<size_t>(fd) >= connections_.size() || !connections_[fd])
        {
            return;
        }

        connection* conn = connections_[fd];
        conn->last_activity = get_current_time();

        if (conn->write_pos == 0)
        {
            modify_epoll(fd, EPOLLIN | EPOLLET);
            return;
        }

        ssize_t total_sent = 0;
        constexpr int max_write_loops = 16;

        for (int i = 0; i < max_write_loops && conn->write_pos > 0; ++i)
        {
            ssize_t n = send(fd, conn->write_buffer, conn->write_pos, MSG_NOSIGNAL);

            if (n > 0)
            {
                total_sent += n;
                if (static_cast<size_t>(n) >= conn->write_pos)
                {
                    conn->write_pos = 0;
                    modify_epoll(fd, EPOLLIN | EPOLLET);
                    break;
                }
                memmove(conn->write_buffer, conn->write_buffer + n, conn->write_pos - n);
                conn->write_pos -= n;
            }
            else if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close_connection(fd);
                ++stats_.errors;
                return;
            }
        }

        if (total_sent > 0)
        {
            total_bytes_written_ += total_sent;
            ++stats_.writes;
        }
    }

    static void process_request_fast(connection* conn)
    {
        if (conn->read_pos == 0) return;

        const size_t copy_size = std::min(static_cast<size_t>(conn->read_pos),
                                          small_buffer_size - conn->write_pos);

        if (copy_size > 0)
        {
            memcpy(conn->write_buffer + conn->write_pos, conn->read_buffer, copy_size);
            conn->write_pos += copy_size;
        }

        conn->read_pos = 0;
    }

    void cleanup_connection_resources(connection* conn) const
    {
        if (!conn) return;

        if (conn->read_buffer)
        {
            read_buffer_pool_->return_buffer(conn->read_buffer);
            conn->read_buffer = nullptr;
        }

        if (conn->write_buffer)
        {
            write_buffer_pool_->return_buffer(conn->write_buffer);
            conn->write_buffer = nullptr;
        }

        connection_pool_->return_connection(conn);
    }

    void close_connection(const int fd)
    {
        if (static_cast<size_t>(fd) >= connections_.size() || !connections_[fd])
        {
            return;
        }

        connection* conn = connections_[fd];
        connections_[fd] = nullptr;

        remove_from_epoll(fd);
        close(fd);
        cleanup_connection_resources(conn);

        --connection_count_;
        ++stats_.closes;
    }

    void cleanup_all_connections()
    {
        size_t cleaned = 0;
        for (size_t fd = 0; fd < connections_.size(); ++fd)
        {
            if (connections_[fd])
            {
                close_connection(static_cast<int>(fd));
                cleaned++;
            }
        }

        if (cleaned > 0)
        {
            std::cout << "Closed " << cleaned << " active connections during shutdown" << std::endl;
        }
    }

    void cleanup_idle_connections()
    {
        const uint32_t current_time = get_current_time();
        size_t cleaned = 0;
        constexpr size_t max_cleanup = cleanup_batch_size;

        for (size_t fd = 0; fd < connections_.size() && cleaned < max_cleanup; ++fd)
        {
            if (const connection* conn = connections_[fd]; conn && conn->fd >= 0)
            {
                if (constexpr uint32_t timeout = 300; current_time - conn->last_activity > timeout)
                {
                    close_connection(static_cast<int>(fd));
                    cleaned++;
                }
            }
        }

        if (cleaned > 0)
        {
            std::cout << " Cleaned " << cleaned << " idle connections" << std::endl;
        }
    }

    static uint32_t get_current_time()
    {
        return static_cast<uint32_t>(time(nullptr));
    }

    static void optimize_system_settings()
    {
        if (setpriority(PRIO_PROCESS, 0, -10) == 0)
        {
            std::cout << " Process priority set to high" << std::endl;
        }

        if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
        {
            std::cout << " Memory pages locked (no swap)" << std::endl;
        }

        check_system_limits();
    }

    static void check_system_limits()
    {
        rlimit rlim{};

        if (getrlimit(RLIMIT_NOFILE, &rlim) == 0)
        {
            std::cout << " File descriptor limit: " << rlim.rlim_cur << " / " << rlim.rlim_max << std::endl;
            if (rlim.rlim_cur < 1048576)
            {
                std::cout << " Recommend: ulimit -n 1048576" << std::endl;
            }
        }
    }

    static void print_optimizations()
    {
        std::cout << "   Leak-safe optimizations enabled:" << std::endl;
        std::cout << "   • RAII smart pointer memory management" << std::endl;
        std::cout << "   • Automatic resource cleanup" << std::endl;
        std::cout << "   • Memory pool validation" << std::endl;
        std::cout << "   • Safe buffer allocation/deallocation" << std::endl;
        std::cout << "   • Connection pool integrity checking" << std::endl;
        std::cout << "   • Exception-safe initialization" << std::endl;
        std::cout << std::endl;
    }

    void print_memory_info() const
    {
        std::cout << "   Memory pool status:" << std::endl;
        std::cout << "   • Read buffers allocated: " << read_buffer_pool_->get_total_allocated() << " bytes" <<
            std::endl;
        std::cout << "   • Write buffers allocated: " << write_buffer_pool_->get_total_allocated() << " bytes" <<
            std::endl;
        std::cout << "   • Connections allocated: " << connection_pool_->get_total_allocated() << " objects" <<
            std::endl;
        std::cout << std::endl;
    }

    void check_memory_health() const
    {
        const size_t memory_kb = get_memory_usage() / 1024;

        std::cout << "   Memory health check:" << std::endl;
        std::cout << "   • Current memory usage: " << memory_kb << "MB" << std::endl;
        std::cout << "   • Active connections: " << connection_count_ << std::endl;
        std::cout << "   • Memory allocation failures: " << stats_.memory_alloc_fails << std::endl;

        if (stats_.memory_alloc_fails > 0)
        {
            std::cout << "  Warning: " << stats_.memory_alloc_fails << " memory allocation failures detected" <<
                std::endl;
        }
    }

    void print_stats() const
    {
        timeval now{};
        gettimeofday(&now, nullptr);

        const long ms = (now.tv_sec - start_time_.tv_sec) * 1000 +
            (now.tv_usec - start_time_.tv_usec) / 1000;

        const size_t memory_mb = get_memory_usage() / 1024;
        const double rate = static_cast<double>(connection_count_) * 1000.0 / static_cast<double>(ms);

        std::cout << " Connections: " << connection_count_
            << ", Memory: " << memory_mb << "MB"
            << ", Time: " << ms << "ms"
            << ", Rate: " << std::fixed << std::setprecision(2) << rate << " conn/s"
            << std::endl;
    }

    void print_performance_stats() const
    {
        std::cout << "⚡ Performance - "
            << "Accepts: " << stats_.accepts
            << ", Reads: " << stats_.reads
            << ", Writes: " << stats_.writes
            << ", Closes: " << stats_.closes
            << ", Errors: " << stats_.errors
            << ", Alloc Fails: " << stats_.memory_alloc_fails
            << ", Data: ↓" << (total_bytes_read_ / 1024 / 1024) << "MB"
            << " ↑" << (total_bytes_written_ / 1024 / 1024) << "MB"
            << std::endl;
    }

    static size_t get_memory_usage()
    {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line))
        {
            if (line.substr(0, 6) == "VmRSS:")
            {
                std::stringstream ss(line);
                std::string key, value, unit;
                ss >> key >> value >> unit;
                return std::stoul(value);
            }
        }
        return 0;
    }
};

int main()
{
    try
    {
        leak_safe_high_perf_server server;

        for (uint16_t port = 2048; port < 2068; ++port)
        {
            server.add_listen_port(port);
        }

        std::cout << " Target: "<< leak_safe_high_perf_server::max_connections <<" concurrent connections" << std::endl;
        std::cout << "  Memory leak protection enabled" << std::endl;
        std::cout << std::endl;

        signal(SIGINT, [](int)
        {
            std::cout << "\n Shutting down gracefully..." << std::endl;
            exit(0);
        });

        server.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << " Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
