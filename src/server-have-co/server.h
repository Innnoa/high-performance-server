#ifndef SERVER_H
#define SERVER_H

#include <sys/epoll.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "memory_pool.h"

class Coroutine;

enum class ConnectionMode { NORMAL_IO, COROUTINE_IO };

class LeakSafeHighPerfServer {
 public:
  static constexpr size_t buffer_size_ = 256;
  static constexpr size_t max_events_ = 8192;
  static constexpr int backlog_ = 1024;
  static constexpr size_t max_connections_ = 1000000;
  static constexpr size_t pool_chunk_size_ = 10000;
  static constexpr size_t max_coroutine_connections_ = 100000;
  static constexpr size_t coroutine_memory_threshold_mb_ = 3072;
  static constexpr size_t simple_conn_threshold_ = 20000;

  LeakSafeHighPerfServer();
  ~LeakSafeHighPerfServer();
  void add_listen_port(uint16_t port);
  void run();
  void stop();

 private:
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

  class FileDescriptor {
    int fd_;

   public:
    explicit FileDescriptor(const int fd) : fd_(fd) {}
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

  void initialize_pools();
  void initialize_connections_container();

  static void configure_socket_options(int fd);
  static void bind_and_listen(int fd, uint16_t port);

  void add_to_epoll(int fd, uint32_t events) const;
  void remove_from_epoll(int fd) const;

  [[nodiscard]] bool is_listen_socket(int fd) const;
  void handle_event(const epoll_event& event);
  void accept_connections(int listen_fd);

  [[nodiscard]] ConnectionMode decide_connection_mode() const;
  bool setup_connection(int fd);
  bool setup_coroutine_connection(int fd);
  bool setup_normal_connection(int fd);
  void handle_normal_io_connection(Connection* conn, uint32_t events);

  Connection* get_connection(int fd);
  void add_connection(int fd, Connection* conn);
  void remove_connection(int fd);

  void close_connection(int fd);
  void cleanup_connection_resources(Connection* conn) const;
  void cleanup_all_connections();
  void cleanup_idle_connections();

  static void connection_coroutine_handler(void* arg);

  static size_t get_system_memory_mb();
  static uint32_t get_current_time();
  static size_t get_memory_usage();

  static void optimize_system_settings();
  static void check_system_limits();

  void print_system_info() const;
  void print_mixed_stats() const;

  FileDescriptor epfd_;

  std::unordered_map<int, std::unique_ptr<Connection>> connections_;

  std::vector<std::unique_ptr<FileDescriptor>> listen_sockets_;
  std::unordered_set<int> listen_socket_fds_;

  std::unique_ptr<BufferPool> read_buffer_pool_;
  std::unique_ptr<BufferPool> write_buffer_pool_;
  std::unique_ptr<ObjectPool<Connection>> connection_pool_;

  std::atomic<bool> running_{true};
  std::atomic<uint64_t> connection_count_{0};
  std::atomic<uint64_t> coroutine_connection_count_{0};
  std::atomic<uint64_t> total_bytes_read_{0};
  std::atomic<uint64_t> total_bytes_written_{0};

  timeval start_time_{};

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