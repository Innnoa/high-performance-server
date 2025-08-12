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
#include <ranges>
#include <sstream>
#include <thread>

#include "coroutine.h"
#include "logger.h"

LeakSafeHighPerfServer::LeakSafeHighPerfServer()
    : epfd_(epoll_create1(EPOLL_CLOEXEC)) {
  if (epfd_.get() < 0) {
    throw std::runtime_error("创建epoll失败");
  }

  initialize_pools();
  initialize_connections_container();

  signal(SIGPIPE, SIG_IGN);
  gettimeofday(&start_time_, nullptr);
  optimize_system_settings();

  Logger::info("高性能服务器已初始化");
  print_system_info();
  HookSystem::initialize();
}

LeakSafeHighPerfServer::~LeakSafeHighPerfServer() {
  Logger::info("服务器关闭 - 清理资源...");

  static bool cleanup_done = false;
  if (!cleanup_done) {
    cleanup_done = true;
    cleanup_all_connections();
  }

  Logger::info("所有资源已清理完成");
}

void LeakSafeHighPerfServer::initialize_pools() {
  try {
    read_buffer_pool_ =
        std::make_unique<BufferPool>(buffer_size_, pool_chunk_size_);
    write_buffer_pool_ =
        std::make_unique<BufferPool>(buffer_size_, pool_chunk_size_);
    connection_pool_ =
        std::make_unique<ObjectPool<Connection>>(pool_chunk_size_);
  } catch (const std::exception& e) {
    throw std::runtime_error("初始化内存池失败 " + std::string(e.what()));
  }
}

void LeakSafeHighPerfServer::initialize_connections_container() {
  try {
    connections_.reserve(max_connections_ / 10);
    listen_socket_fds_.reserve(32);
    Logger::info("使用预留空间初始化连接容器");
  } catch (const std::bad_alloc&) {
    Logger::info("连接容器初始化时没有预留（内存不足）");
  }
}

LeakSafeHighPerfServer::Connection* LeakSafeHighPerfServer::get_connection(
    const int fd) {
  if (const auto it = connections_.find(fd);
      it != connections_.end() && it->second) {
    return it->second.get();
  }
  return nullptr;
}

void LeakSafeHighPerfServer::add_connection(const int fd, Connection* conn) {
  if (conn) {
    connections_[fd] = std::unique_ptr<Connection>(conn);
  }
}

void LeakSafeHighPerfServer::remove_connection(const int fd) {
  if (const auto it = connections_.find(fd); it != connections_.end()) {
    if (const auto pointer = it->second.release(); pointer == nullptr) {
      // Logger::info("成功释放");
    }
    connections_.erase(it);
  }
}

void LeakSafeHighPerfServer::add_listen_port(const uint16_t port) {
  int sockfd_raw =
      socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (sockfd_raw < 0) {
    throw std::runtime_error("socket 构建失败");
  }

  auto sockfd = std::make_unique<FileDescriptor>(sockfd_raw);
  configure_socket_options(sockfd->get());
  bind_and_listen(sockfd->get(), port);
  add_to_epoll(sockfd->get(), EPOLLIN | EPOLLET);

  listen_socket_fds_.insert(sockfd->get());
  listen_sockets_.push_back(std::move(sockfd));

  Logger::info("监听端口于: " + std::to_string(port));
}

void LeakSafeHighPerfServer::run() {
  std::array<epoll_event, max_events_> events{};
  auto last_cleanup = std::chrono::steady_clock::now();

  Logger::info("混合 IO 模式启动 (协程限制量: " +
               std::to_string(max_coroutine_connections_) + ")");
  print_mixed_stats();

  auto& scheduler = Scheduler::instance();

  while (running_) {
    const int nfds = epoll_wait(epfd_.get(), events.data(), max_events_, 10);

    if (nfds < 0) {
      if (errno == EINTR) continue;
      Logger::error("epoll_wait 失败: " + std::string(strerror(errno)));
      break;
    }

    for (int i = 0; i < nfds; ++i) {
      handle_event(events[i]);
    }

    scheduler.schedule_once();

    // 定期清理
    if (auto now = std::chrono::steady_clock::now();
        std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup)
            .count() >= 30) {
      cleanup_idle_connections();
      last_cleanup = now;
    }
  }
}

void LeakSafeHighPerfServer::stop() { running_ = false; }

void LeakSafeHighPerfServer::configure_socket_options(const int fd) {
  constexpr int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  constexpr int buf_size = 65536;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
}

void LeakSafeHighPerfServer::bind_and_listen(const int fd,
                                             const uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    throw std::runtime_error("绑定socket失败");
  }

  if (listen(fd, backlog_) < 0) {
    throw std::runtime_error("监听socket失败");
  }
}

// ReSharper disable once CppDFAConstantParameter
void LeakSafeHighPerfServer::add_to_epoll(const int fd,
                                          const uint32_t events) const {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;

  if (epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
    throw std::runtime_error("添加fd至epoll失败");
  }
}

void LeakSafeHighPerfServer::remove_from_epoll(const int fd) const {
  epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
}

bool LeakSafeHighPerfServer::is_listen_socket(const int fd) const {
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
    if (Connection* conn = get_connection(fd);
        conn && !conn->coroutine_active) {
      handle_normal_io_connection(conn, event.events);
    }
  }
}

void LeakSafeHighPerfServer::accept_connections(const int listen_fd) {
  constexpr int max_batch = 200;

  for (int accepted = 0; accepted < max_batch; ++accepted) {
    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    const int client_fd =
        accept4(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len,
                SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EMFILE || errno == ENFILE) {
        Logger::error("达到文件描述符限制!");
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

ConnectionMode LeakSafeHighPerfServer::decide_connection_mode() const {
  if (coroutine_connection_count_.load() >= max_coroutine_connections_) {
    return ConnectionMode::NORMAL_IO;
  }

  if (const size_t current_memory_mb = get_memory_usage() / 1024;
      current_memory_mb > coroutine_memory_threshold_mb_) {
    return ConnectionMode::NORMAL_IO;
  }
  if (connection_count_.load() > simple_conn_threshold_) {
    return connection_count_.load() % 5 == 0 ? ConnectionMode::COROUTINE_IO
                                               : ConnectionMode::NORMAL_IO;
  }

  return ConnectionMode::COROUTINE_IO;
}

bool LeakSafeHighPerfServer::setup_connection(const int fd) {
  if (connection_count_ >= max_connections_) {
    Logger::error("达到文件描述符限制: " + std::to_string(max_connections_));
    return false;
  }

  if (get_connection(fd) != nullptr) {
    Logger::error("该fd已经存在: " + std::to_string(fd));
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

bool LeakSafeHighPerfServer::setup_coroutine_connection(const int fd) {
  if (coroutine_connection_count_.load() >= max_coroutine_connections_) {
    return setup_normal_connection(fd);
  }

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

  conn->fd = fd;
  conn->last_activity = get_current_time();
  conn->server = this;

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

  if (connection_count_ % 2000 == 0) {
    print_mixed_stats();
  }

  return true;
}

bool LeakSafeHighPerfServer::setup_normal_connection(const int fd) {
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

  conn->fd = fd;
  conn->last_activity = get_current_time();
  conn->coroutine_active = false;
  conn->server = this;

  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  add_connection(fd, conn);
  ++connection_count_;
  ++stats_.accepts;

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

void LeakSafeHighPerfServer::handle_normal_io_connection(
    Connection* conn, const uint32_t events) {
  if (!conn || conn->fd < 0) return;

  try {
    if (events & EPOLLIN) {
      char buffer[256];

      if (const ssize_t n = read(conn->fd, buffer, sizeof(buffer)); n > 0) {
        if (const ssize_t written = write(conn->fd, buffer, n); written > 0) {
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

void LeakSafeHighPerfServer::close_connection(const int fd) {
  Connection* conn = get_connection(fd);
  if (!conn) {
    return;
  }

  if (conn->fd == -1) {
    Logger::debug("该fd连接已经关闭: " + std::to_string(fd));
    return;
  }

  conn->fd = -1;

  if (conn->coroutine_active) {
    --coroutine_connection_count_;
    conn->coroutine_active = false;
    if (conn->coroutine) {
      conn->coroutine->set_state(CoroutineState::FINISHED);
      conn->coroutine = nullptr;  // 清空指针
    }
  }

  remove_from_epoll(fd);

  if (fd >= 0) {
    close(fd);
  }

  cleanup_connection_resources(conn);

  remove_connection(fd);

  --connection_count_;
  ++stats_.closes;
}

void LeakSafeHighPerfServer::cleanup_connection_resources(
    Connection* conn) const {
  if (!conn) return;

  if (conn->read_buffer) {
    read_buffer_pool_->return_buffer(conn->read_buffer);
    conn->read_buffer = nullptr;
  }

  if (conn->write_buffer) {
    write_buffer_pool_->return_buffer(conn->write_buffer);
    conn->write_buffer = nullptr;
  }

  conn->coroutine = nullptr;
  conn->server = nullptr;
  conn->coroutine_active = false;

  connection_pool_->return_object(conn);
}

void LeakSafeHighPerfServer::cleanup_all_connections() {
  Logger::info("清理服务器连接...");

  size_t cleaned = 0;
  std::vector<int> fds_to_close;

  for (const auto& key : connections_ | std::views::keys) {
    fds_to_close.push_back(key);
  }

  Logger::info("找到 " + std::to_string(fds_to_close.size()) + " 连接需清理");

  constexpr size_t batch_size = 1000;
  for (size_t i = 0; i < fds_to_close.size(); i += batch_size) {
    const size_t end = std::min(i + batch_size, fds_to_close.size());

    for (size_t j = i; j < end; ++j) {
      close_connection(fds_to_close[j]);
      cleaned++;
    }

    if (end < fds_to_close.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  if (!connections_.empty()) {
    Logger::error("警告: " + std::to_string(connections_.size()) +
                  " 清除后连接仍然存在");
    connections_.clear();  // 强制清空
  }

  if (cleaned > 0) {
    Logger::info("关闭 " + std::to_string(cleaned) + "个服务器连接");
  }
}

void LeakSafeHighPerfServer::cleanup_idle_connections() {
  const uint32_t current_time = get_current_time();
  size_t cleaned = 0;

  std::vector<int> fds_to_close;

  for (const auto& [fst, snd] : connections_) {
    if (constexpr size_t max_cleanup = 1000; cleaned >= max_cleanup) break;

    const int fd = fst;
    const Connection* conn = snd.get();

    if (constexpr uint32_t timeout = 300;
        conn && conn->fd >= 0 && current_time - conn->last_activity > timeout) {
      fds_to_close.push_back(fd);
      cleaned++;
    }
  }

  for (const int fd : fds_to_close) {
    close_connection(fd);
  }

  if (cleaned > 0) {
    Logger::info("已清理 " + std::to_string(cleaned) + " 空闲连接");
  }
}

void LeakSafeHighPerfServer::connection_coroutine_handler(void* arg) {
  auto* conn = static_cast<Connection*>(arg);
  auto* server = conn->server;
  char buffer[256];

  try {
    while (conn->coroutine_active) {
      const ssize_t n = read(conn->fd, buffer, sizeof(buffer));

      if (n <= 0) {
        break;
      }

      const ssize_t written = write(conn->fd, buffer, n);
      if (written != n) {
        break;
      }

      ++server->stats_.reads;
      ++server->stats_.writes;
      server->total_bytes_read_ += n;
      server->total_bytes_written_ += written;
    }
  } catch (...) {
  }

  conn->coroutine_active = false;
}

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
    Logger::info("文件描述符限制: " + std::to_string(rlim.rlim_cur) + " / " +
                 std::to_string(rlim.rlim_max));
    if (rlim.rlim_cur < 1048576) {
      Logger::info("提示: ulimit -n 1048576");
    }
  }
}

void LeakSafeHighPerfServer::print_system_info() const {
  const size_t read_pool_mb =
      read_buffer_pool_->get_total_allocated() / (1024 * 1024);
  const size_t write_pool_mb =
      write_buffer_pool_->get_total_allocated() / (1024 * 1024);
  const size_t total_memory_mb = read_pool_mb + write_pool_mb;

  Logger::info("内存池已初始化:");
  Logger::info("•读内存池大小: " + std::to_string(read_pool_mb) + "MB");
  Logger::info("•写内存池大小: " + std::to_string(write_pool_mb) + "MB");
  Logger::info(
      "•连接池: " + std::to_string(connection_pool_->get_total_allocated()) +
      " 实体");
  Logger::info("•总分配: " + std::to_string(total_memory_mb) + "MB");
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

  std::cout << "混合模式 - 统计: " << connection_count_
            << ", 正常: " << normal_connections << ", Memory: " << memory_mb
            << "MB"
            << ", 速率: " << std::fixed << std::setprecision(1) << rate
            << " conn/s" << std::endl;
}