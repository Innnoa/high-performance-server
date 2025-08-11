#ifndef SERVER_H
#define SERVER_H

#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "memory_pool.h"

// 前向声明
class Coroutine;

// 连接模式枚举
enum class ConnectionMode {
  NORMAL_IO,    // 普通同步IO
  COROUTINE_IO  // 协程异步IO
};

class LeakSafeHighPerfServer {
 public:
  // 核心常量配置
  static constexpr size_t BUFFER_SIZE = 256;
  static constexpr size_t MAX_EVENTS = 8192;
  static constexpr int BACKLOG = 1024;
  static constexpr size_t MAX_CONNECTIONS = 1000000;
  static constexpr size_t POOL_CHUNK_SIZE = 10000;

  // 混合模式配置
  static constexpr size_t MAX_COROUTINE_CONNECTIONS = 100000;
  static constexpr size_t COROUTINE_MEMORY_THRESHOLD_MB = 3072;
  static constexpr size_t SIMPLE_CONN_THRESHOLD = 20000;

  // 构造/析构
  LeakSafeHighPerfServer();
  ~LeakSafeHighPerfServer();

  // 公共接口
  void add_listen_port(uint16_t port);
  void run();
  void stop();

 private:
  // 连接结构体
  struct Connection {
    int fd = -1;
    uint32_t last_activity = 0;
    uint16_t read_pos = 0;
    uint16_t write_pos = 0;
    char* read_buffer = nullptr;
    char* write_buffer = nullptr;
    Coroutine* coroutine = nullptr;
    bool coroutine_active = false;
    LeakSafeHighPerfServer* server = nullptr;
  };

  // RAII文件描述符
  class FileDescriptor {
    int fd_;

   public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() {
      if (fd_ >= 0) close(fd_);
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
      other.fd_ = -1;
    }
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
      if (this != &other) {
        if (fd_ >= 0) close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
      }
      return *this;
    }

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
  };

  // 核心功能
  void initialize_pools();
  void initialize_connections_container();

  // 网络配置
  static void configure_socket_options(int fd);
  static void bind_and_listen(int fd, uint16_t port);

  // epoll管理
  void add_to_epoll(int fd, uint32_t events) const;
  void remove_from_epoll(int fd) const;

  // 事件处理
  [[nodiscard]] bool is_listen_socket(int fd) const;
  void handle_event(const epoll_event& event);
  void accept_connections(int listen_fd);

  // 连接管理 - 混合模式
  [[nodiscard]] ConnectionMode decide_connection_mode() const;
  bool setup_connection(int fd);
  bool setup_coroutine_connection(int fd);
  bool setup_normal_connection(int fd);
  void handle_normal_io_connection(Connection* conn, uint32_t events);

  // 连接查找和管理
  Connection* get_connection(int fd);
  void add_connection(int fd, Connection* conn);
  void remove_connection(int fd);

  // 连接清理
  void close_connection(int fd);
  void cleanup_connection_resources(Connection* conn) const;
  void cleanup_all_connections();
  void cleanup_idle_connections();

  // 协程处理函数
  static void connection_coroutine_handler(void* arg);

  // 工具函数
  static size_t get_system_memory_mb();
  static uint32_t get_current_time();
  static size_t get_memory_usage();

  // 系统优化
  static void optimize_system_settings();
  static void check_system_limits();

  // 统计和监控
  void print_system_info() const;
  void print_mixed_stats() const;

  // 成员变量
  FileDescriptor epfd_;

  // 方案1: 使用unordered_map (推荐)
  std::unordered_map<int, std::unique_ptr<Connection>> connections_;

  // 方案2: 如果需要更高性能，可以使用以下组合:
  // std::unordered_map<int, Connection*> connections_;
  // std::unordered_set<int> active_fds_;  // 用于快速遍历活跃连接

  std::vector<std::unique_ptr<FileDescriptor>> listen_sockets_;
  std::unordered_set<int> listen_socket_fds_;  // 用于快速判断是否为监听socket

  // 内存池（使用新的模块化设计）
  std::unique_ptr<BufferPool> read_buffer_pool_;
  std::unique_ptr<BufferPool> write_buffer_pool_;
  std::unique_ptr<ObjectPool<Connection>> connection_pool_;

  // 原子计数器
  std::atomic<bool> running_{true};
  std::atomic<uint64_t> connection_count_{0};
  std::atomic<uint64_t> coroutine_connection_count_{0};
  std::atomic<uint64_t> total_bytes_read_{0};
  std::atomic<uint64_t> total_bytes_written_{0};

  timeval start_time_{};

  // 统计结构
  struct Statistics {
    std::atomic<uint64_t> accepts{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> closes{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> memory_alloc_fails{0};
  } stats_;
};

#endif  // SERVER_H