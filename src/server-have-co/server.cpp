#include "server.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#include "coroutine.h"
#include "logger.h"

// ============================================================================
// 构造函数和析构函数
// ============================================================================

LeakSafeHighPerfServer::LeakSafeHighPerfServer()
    : epfd_(epoll_create1(EPOLL_CLOEXEC)) {
  if (epfd_.get() < 0) {
    throw std::runtime_error("Failed to create epoll");
  }

  initialize_pools();
  initialize_connections_container();

  signal(SIGPIPE, SIG_IGN);
  gettimeofday(&start_time_, nullptr);
  optimize_system_settings();

  Logger::info("High-performance server initialized");
  print_system_info();
  HookSystem::initialize();
}

LeakSafeHighPerfServer::~LeakSafeHighPerfServer() {
  Logger::info("Server shutdown - cleaning up resources...");

  // 确保只清理一次
  static bool cleanup_done = false;
  if (!cleanup_done) {
    cleanup_done = true;
    cleanup_all_connections();
  }

  Logger::info("All resources cleaned up successfully");
}

// ============================================================================
// 初始化方法
// ============================================================================

void LeakSafeHighPerfServer::initialize_pools() {
  try {
    read_buffer_pool_ =
        std::make_unique<BufferPool>(BUFFER_SIZE, POOL_CHUNK_SIZE);
    write_buffer_pool_ =
        std::make_unique<BufferPool>(BUFFER_SIZE, POOL_CHUNK_SIZE);
    connection_pool_ =
        std::make_unique<ObjectPool<Connection>>(POOL_CHUNK_SIZE);
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to initialize memory pools: " +
                             std::string(e.what()));
  }
}

void LeakSafeHighPerfServer::initialize_connections_container() {
  try {
    // 预分配一些空间以减少rehash
    connections_.reserve(MAX_CONNECTIONS / 10);  // 预留10%的空间
    listen_socket_fds_.reserve(32);              // 预留32个监听socket空间
    Logger::info("Connection container initialized with reserve space");
  } catch (const std::bad_alloc&) {
    Logger::info(
        "Connection container initialized without reserve (low memory)");
  }
}

// ============================================================================
// 连接查找和管理方法
// ============================================================================

LeakSafeHighPerfServer::Connection* LeakSafeHighPerfServer::get_connection(
    int fd) {
  auto it = connections_.find(fd);
  if (it != connections_.end() && it->second) {
    return it->second.get();
  }
  return nullptr;
}

void LeakSafeHighPerfServer::add_connection(int fd, Connection* conn) {
  if (conn) {
    connections_[fd] = std::unique_ptr<Connection>(conn);
  }
}

void LeakSafeHighPerfServer::remove_connection(int fd) {
  auto it = connections_.find(fd);
  if (it != connections_.end()) {
    // unique_ptr会自动处理析构，但我们已经手动清理了资源
    it->second.release();  // 释放所有权，避免重复delete
    connections_.erase(it);
  }
}

// ============================================================================
// 公共接口实现
// ============================================================================

void LeakSafeHighPerfServer::add_listen_port(uint16_t port) {
  int sockfd_raw =
      socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (sockfd_raw < 0) {
    throw std::runtime_error("Failed to create socket");
  }

  auto sockfd = std::make_unique<FileDescriptor>(sockfd_raw);
  configure_socket_options(sockfd->get());
  bind_and_listen(sockfd->get(), port);
  add_to_epoll(sockfd->get(), EPOLLIN | EPOLLET);

  // 添加到监听socket集合
  listen_socket_fds_.insert(sockfd->get());
  listen_sockets_.push_back(std::move(sockfd));

  Logger::info("Listening on port " + std::to_string(port));
}

void LeakSafeHighPerfServer::run() {
  std::array<epoll_event, MAX_EVENTS> events{};
  auto last_cleanup = std::chrono::steady_clock::now();

  Logger::info("🔄 Mixed IO mode started (coroutine limit: " +
               std::to_string(MAX_COROUTINE_CONNECTIONS) + ")");
  print_mixed_stats();

  auto& scheduler = Scheduler::instance();

  while (running_) {
    const int nfds = epoll_wait(epfd_.get(), events.data(), MAX_EVENTS, 10);

    if (nfds < 0) {
      if (errno == EINTR) continue;
      Logger::error("epoll_wait failed: " + std::string(strerror(errno)));
      break;
    }

    for (int i = 0; i < nfds; ++i) {
      handle_event(events[i]);
    }

    scheduler.schedule_once();

    // 定期清理
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup)
            .count() >= 30) {
      cleanup_idle_connections();
      last_cleanup = now;
    }
  }
}

void LeakSafeHighPerfServer::stop() { running_ = false; }

// ============================================================================
// 网络配置方法
// ============================================================================

void LeakSafeHighPerfServer::configure_socket_options(int fd) {
  constexpr int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  constexpr int buf_size = 65536;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
}

void LeakSafeHighPerfServer::bind_and_listen(int fd, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    throw std::runtime_error("Failed to bind socket");
  }

  if (listen(fd, BACKLOG) < 0) {
    throw std::runtime_error("Failed to listen on socket");
  }
}

// ============================================================================
// epoll管理方法
// ============================================================================

// ReSharper disable once CppDFAConstantParameter
void LeakSafeHighPerfServer::add_to_epoll(int fd, const uint32_t events) const {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;

  if (epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
    throw std::runtime_error("Failed to add fd to epoll");
  }
}

void LeakSafeHighPerfServer::remove_from_epoll(int fd) const {
  epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
}

// ============================================================================
// 事件处理方法
// ============================================================================

bool LeakSafeHighPerfServer::is_listen_socket(int fd) const {
  return listen_socket_fds_.contains(fd);
}

void LeakSafeHighPerfServer::handle_event(const epoll_event& event) {
  const int fd = event.data.fd;

  if (event.events & (EPOLLERR | EPOLLHUP)) {
    if (!is_listen_socket(fd)) {
      close_connection(fd);
      ++stats_.errors;
    }
    return;
  }

  if (is_listen_socket(fd)) {
    if (event.events & EPOLLIN) {
      accept_connections(fd);
    }
  } else {
    // 处理客户端连接事件
    Connection* conn = get_connection(fd);
    if (conn && !conn->coroutine_active) {
      handle_normal_io_connection(conn, event.events);
    }
    // 协程连接的事件由协程系统自动处理
  }
}

void LeakSafeHighPerfServer::accept_connections(int listen_fd) {
  constexpr int MAX_BATCH = 200;

  for (int accepted = 0; accepted < MAX_BATCH; ++accepted) {
    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    int client_fd =
        accept4(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len,
                SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EMFILE || errno == ENFILE) {
        Logger::error("File descriptor limit reached!");
        cleanup_idle_connections();
        break;
      }
      ++stats_.errors;
      break;
    }

    if (!setup_connection(client_fd)) {
      close(client_fd);
      break;
    }
  }
}

// ============================================================================
// 混合模式连接管理
// ============================================================================

ConnectionMode LeakSafeHighPerfServer::decide_connection_mode() const {
  // 检查协程连接数限制
  if (coroutine_connection_count_.load() >= MAX_COROUTINE_CONNECTIONS) {
    return ConnectionMode::NORMAL_IO;
  }

  // 检查内存使用
  const size_t current_memory_mb = get_memory_usage() / 1024;
  if (current_memory_mb > COROUTINE_MEMORY_THRESHOLD_MB) {
    return ConnectionMode::NORMAL_IO;
  }

  // 高负载时混合使用（20%协程）
  if (connection_count_.load() > SIMPLE_CONN_THRESHOLD) {
    return (connection_count_.load() % 5 == 0) ? ConnectionMode::COROUTINE_IO
                                               : ConnectionMode::NORMAL_IO;
  }

  return ConnectionMode::COROUTINE_IO;
}

bool LeakSafeHighPerfServer::setup_connection(int fd) {
  if (connection_count_ >= MAX_CONNECTIONS) {
    Logger::error("Connection limit reached: " +
                  std::to_string(MAX_CONNECTIONS));
    return false;
  }

  // 检查fd是否已经存在（避免重复添加）
  if (get_connection(fd) != nullptr) {
    Logger::error("Connection already exists for fd: " + std::to_string(fd));
    return false;
  }

  switch (decide_connection_mode()) {
    case ConnectionMode::COROUTINE_IO:
      return setup_coroutine_connection(fd);
    case ConnectionMode::NORMAL_IO:
      return setup_normal_connection(fd);
  }

  return false;
}

bool LeakSafeHighPerfServer::setup_coroutine_connection(int fd) {
  // 最后检查协程限制
  if (coroutine_connection_count_.load() >= MAX_COROUTINE_CONNECTIONS) {
    return setup_normal_connection(fd);
  }

  // 获取连接对象和缓冲区
  Connection* conn = connection_pool_->get_object();
  if (!conn) {
    ++stats_.memory_alloc_fails;
    return false;
  }

  conn->read_buffer = read_buffer_pool_->get_buffer();
  conn->write_buffer = write_buffer_pool_->get_buffer();

  if (!conn->read_buffer || !conn->write_buffer) {
    if (conn->read_buffer) read_buffer_pool_->return_buffer(conn->read_buffer);
    if (conn->write_buffer)
      write_buffer_pool_->return_buffer(conn->write_buffer);
    connection_pool_->return_object(conn);
    ++stats_.memory_alloc_fails;
    return false;
  }

  // 初始化连接
  conn->fd = fd;
  conn->last_activity = get_current_time();
  conn->server = this;

  // 创建协程
  auto& scheduler = Scheduler::instance();
  try {
    conn->coroutine =
        scheduler.create_coroutine(connection_coroutine_handler, conn);
  } catch (const std::bad_alloc&) {
    cleanup_connection_resources(conn);
    ++stats_.memory_alloc_fails;
    return false;
  }

  if (!conn->coroutine) {
    cleanup_connection_resources(conn);
    return false;
  }

  conn->coroutine_active = true;
  add_connection(fd, conn);
  ++connection_count_;
  ++coroutine_connection_count_;
  ++stats_.accepts;

  // 定期统计
  if (connection_count_ % 2000 == 0) {
    print_mixed_stats();
  }

  return true;
}

bool LeakSafeHighPerfServer::setup_normal_connection(int fd) {
  Connection* conn = connection_pool_->get_object();
  if (!conn) {
    ++stats_.memory_alloc_fails;
    return false;
  }

  conn->read_buffer = read_buffer_pool_->get_buffer();
  if (!conn->read_buffer) {
    connection_pool_->return_object(conn);
    ++stats_.memory_alloc_fails;
    return false;
  }

  // 普通IO连接不需要写缓冲区（直接回显）
  conn->fd = fd;
  conn->last_activity = get_current_time();
  conn->coroutine_active = false;
  conn->server = this;

  // 设置非阻塞
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  add_connection(fd, conn);
  ++connection_count_;
  ++stats_.accepts;

  // 添加到epoll
  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = fd;

  if (epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) == -1) {
    cleanup_connection_resources(conn);
    remove_connection(fd);
    --connection_count_;
    return false;
  }

  if (connection_count_ % 2000 == 0) {
    print_mixed_stats();
  }

  return true;
}

void LeakSafeHighPerfServer::handle_normal_io_connection(Connection* conn,
                                                         uint32_t events) {
  if (!conn || conn->fd < 0) return;

  try {
    if (events & EPOLLIN) {
      char buffer[256];
      ssize_t n = ::read(conn->fd, buffer, sizeof(buffer));

      if (n > 0) {
        ssize_t written = ::write(conn->fd, buffer, n);
        if (written > 0) {
          ++stats_.reads;
          ++stats_.writes;
          total_bytes_read_ += n;
          total_bytes_written_ += written;
          conn->last_activity = get_current_time();
        }
      } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        close_connection(conn->fd);
      }
    }

    if (events & (EPOLLHUP | EPOLLERR)) {
      close_connection(conn->fd);
    }
  } catch (const std::exception& e) {
    Logger::error("Normal IO connection error, fd: " +
                  std::to_string(conn->fd) + ", error: " + e.what());
    close_connection(conn->fd);
  }
}

// ============================================================================
// 连接清理
// ============================================================================

void LeakSafeHighPerfServer::close_connection(int fd) {
  Connection* conn = get_connection(fd);
  if (!conn) {
    return;  // 连接已经不存在，直接返回
  }

  // 防止重复清理同一个连接
  if (conn->fd == -1) {
    Logger::debug("Connection already closed for fd: " + std::to_string(fd));
    return;
  }

  // 标记连接为已关闭
  conn->fd = -1;

  if (conn->coroutine_active) {
    --coroutine_connection_count_;
    conn->coroutine_active = false;
    if (conn->coroutine) {
      conn->coroutine->set_state(CoroutineState::FINISHED);
      conn->coroutine = nullptr;  // 清空指针
    }
  }

  // 从epoll中移除（忽略错误）
  remove_from_epoll(fd);

  // 关闭文件描述符
  if (fd >= 0) {
    close(fd);
  }

  // 清理连接资源
  cleanup_connection_resources(conn);

  // 从容器中移除
  remove_connection(fd);

  --connection_count_;
  ++stats_.closes;
}

void LeakSafeHighPerfServer::cleanup_connection_resources(
    Connection* conn) const {
  if (!conn) return;

  // 安全地返回缓冲区
  if (conn->read_buffer) {
    read_buffer_pool_->return_buffer(conn->read_buffer);
    conn->read_buffer = nullptr;
  }

  if (conn->write_buffer) {
    write_buffer_pool_->return_buffer(conn->write_buffer);
    conn->write_buffer = nullptr;
  }

  // 重置连接状态
  conn->coroutine = nullptr;
  conn->server = nullptr;
  conn->coroutine_active = false;

  // 返回连接对象到池中（这里会调用placement new重新初始化）
  connection_pool_->return_object(conn);
}

void LeakSafeHighPerfServer::cleanup_all_connections() {
  Logger::info("Starting connection cleanup process...");

  size_t cleaned = 0;
  std::vector<int> fds_to_close;

  // 收集所有需要关闭的fd
  for (const auto& pair : connections_) {
    fds_to_close.push_back(pair.first);
  }

  Logger::info("Found " + std::to_string(fds_to_close.size()) +
               " connections to clean");

  // 分批清理，避免一次性操作过多
  constexpr size_t BATCH_SIZE = 1000;
  for (size_t i = 0; i < fds_to_close.size(); i += BATCH_SIZE) {
    size_t end = std::min(i + BATCH_SIZE, fds_to_close.size());

    for (size_t j = i; j < end; ++j) {
      close_connection(fds_to_close[j]);
      cleaned++;
    }

    // 每批处理后短暂休息，让系统缓冲
    if (end < fds_to_close.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // 最终检查确保容器为空
  if (!connections_.empty()) {
    Logger::error("Warning: " + std::to_string(connections_.size()) +
                  " connections remain after cleanup");
    connections_.clear();  // 强制清空
  }

  if (cleaned > 0) {
    Logger::info("Closed " + std::to_string(cleaned) +
                 " connections during shutdown");
  }
}

void LeakSafeHighPerfServer::cleanup_idle_connections() {
  const uint32_t current_time = get_current_time();
  size_t cleaned = 0;
  constexpr size_t max_cleanup = 1000;
  constexpr uint32_t timeout = 300;

  std::vector<int> fds_to_close;

  // 先收集需要关闭的连接
  for (const auto& pair : connections_) {
    if (cleaned >= max_cleanup) break;

    const int fd = pair.first;
    const Connection* conn = pair.second.get();

    if (conn && conn->fd >= 0 && current_time - conn->last_activity > timeout) {
      fds_to_close.push_back(fd);
      cleaned++;
    }
  }

  // 再执行关闭操作
  for (int fd : fds_to_close) {
    close_connection(fd);
  }

  if (cleaned > 0) {
    Logger::info("Cleaned " + std::to_string(cleaned) + " idle connections");
  }
}

// ============================================================================
// 协程处理函数
// ============================================================================

void LeakSafeHighPerfServer::connection_coroutine_handler(void* arg) {
  auto* conn = static_cast<Connection*>(arg);
  auto* server = conn->server;
  char buffer[256];

  try {
    while (conn->coroutine_active) {
      ssize_t n = read(conn->fd, buffer, sizeof(buffer));

      if (n <= 0) {
        break;
      }

      ssize_t written = write(conn->fd, buffer, n);
      if (written != n) {
        break;
      }

      ++server->stats_.reads;
      ++server->stats_.writes;
      server->total_bytes_read_ += n;
      server->total_bytes_written_ += written;
    }
  } catch (...) {
    // 协程异常处理
  }

  conn->coroutine_active = false;
}

// ============================================================================
// 工具方法
// ============================================================================

size_t LeakSafeHighPerfServer::get_system_memory_mb() {
  std::ifstream meminfo("/proc/meminfo");
  if (!meminfo.is_open()) return 8192;

  std::string line;
  while (std::getline(meminfo, line)) {
    if (line.substr(0, 9) == "MemTotal:") {
      std::stringstream ss(line);
      std::string key, value, unit;
      ss >> key >> value >> unit;
      return std::stoul(value) / 1024;
    }
  }
  return 8192;
}

uint32_t LeakSafeHighPerfServer::get_current_time() {
  return static_cast<uint32_t>(time(nullptr));
}

size_t LeakSafeHighPerfServer::get_memory_usage() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.substr(0, 6) == "VmRSS:") {
      std::stringstream ss(line);
      std::string key, value, unit;
      ss >> key >> value >> unit;
      return std::stoul(value);
    }
  }
  return 0;
}

void LeakSafeHighPerfServer::optimize_system_settings() {
  setpriority(PRIO_PROCESS, 0, -10);
  mlockall(MCL_CURRENT | MCL_FUTURE);
  check_system_limits();
}

void LeakSafeHighPerfServer::check_system_limits() {
  rlimit rlim{};
  if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
    Logger::info("File descriptor limit: " + std::to_string(rlim.rlim_cur) +
                 " / " + std::to_string(rlim.rlim_max));
    if (rlim.rlim_cur < 1048576) {
      Logger::info("Recommend: ulimit -n 1048576");
    }
  }
}

// ============================================================================
// 统计和监控
// ============================================================================

void LeakSafeHighPerfServer::print_system_info() const {
  const size_t read_pool_mb =
      read_buffer_pool_->get_total_allocated() / (1024 * 1024);
  const size_t write_pool_mb =
      write_buffer_pool_->get_total_allocated() / (1024 * 1024);
  const size_t total_memory_mb = read_pool_mb + write_pool_mb;

  Logger::info("Memory pools initialized:");
  Logger::info("• Read buffer pool: " + std::to_string(read_pool_mb) + "MB");
  Logger::info("• Write buffer pool: " + std::to_string(write_pool_mb) + "MB");
  Logger::info("• Connection pool: " +
               std::to_string(connection_pool_->get_total_allocated()) +
               " objects");
  Logger::info("• Total pre-allocated: " + std::to_string(total_memory_mb) +
               "MB");
}

void LeakSafeHighPerfServer::print_mixed_stats() const {
  const size_t normal_connections =
      connection_count_ - coroutine_connection_count_;
  const size_t memory_mb = get_memory_usage() / 1024;

  timeval now{};
  gettimeofday(&now, nullptr);
  const long ms = (now.tv_sec - start_time_.tv_sec) * 1000 +
                  (now.tv_usec - start_time_.tv_usec) / 1000;
  const double rate =
      static_cast<double>(connection_count_) * 1000.0 / static_cast<double>(ms);

  std::cout << "Mixed mode - Total: " << connection_count_
            << ", Normal: " << normal_connections << ", Memory: " << memory_mb
            << "MB"
            << ", Rate: " << std::fixed << std::setprecision(1) << rate
            << " conn/s" << std::endl;
}