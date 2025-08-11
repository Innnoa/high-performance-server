#include "coroutine.h"

// 系统相关头文件
#define GNU_SOURCE
#include <dlfcn.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

// ============================================================================
// 静态变量和全局变量定义
// ============================================================================

// Scheduler 的 thread_local 实例
thread_local std::unique_ptr<Scheduler> Scheduler::instance_;

// HookSystem 的静态变量
bool HookSystem::initialized_ = false;

// Hook 函数指针
using read_t = ssize_t (*)(int fd, void* buf, size_t count);
using write_t = ssize_t (*)(int fd, const void* buf, size_t count);

static read_t original_read = nullptr;
static write_t original_write = nullptr;

// ============================================================================
// Hook 系统实现
// ============================================================================

// Hook read 函数 - 简化版
extern "C" ssize_t read(const int fd, void* buf, const size_t count) {
  // 移除init_hooks()调用，假设已经初始化完成

  auto& scheduler = Scheduler::instance();
  if (!scheduler.in_coroutine()) {
    return original_read(fd, buf, count);
  }

  pollfd pfd = {fd, POLLIN, 0};
  if (const int result = poll(&pfd, 1, 0); result <= 0) {
    scheduler.wait_for_read(fd);
  }

  const ssize_t ret = original_read(fd, buf, count);
  if (ret > 0) {
#ifdef DEBUG
    std::cout << "协程读取 " << ret << " 字节" << std::endl;
#endif
  }
  return ret;
}

// Hook write 函数 - 简化版
extern "C" ssize_t write(const int fd, const void* buf, const size_t count) {
  const ssize_t ret = original_write(fd, buf, count);
  if (ret > 0) {
#ifdef DEBUG
    std::cout << "协程写入 " << ret << " 字节" << std::endl;
#endif
  }
  return ret;
}

void HookSystem::initialize() {
  static std::once_flag init_flag;
  std::call_once(init_flag, [] {
    // ReSharper disable once CppCStyleCast
    original_read = (read_t)dlsym(RTLD_NEXT, "read");
    // ReSharper disable once CppCStyleCast
    original_write = (write_t)dlsym(RTLD_NEXT, "write");

    if (original_read && original_write) {
      std::cout << "Hook函数指针初始化完成" << std::endl;
      std::cout << "Hook系统初始化完成" << std::endl;
      initialized_ = true;
    } else {
      std::cerr << "ERROR: Hook函数指针初始化失败" << std::endl;
      std::exit(1);
    }
  });
}

bool HookSystem::is_initialized() { return initialized_; }

// ============================================================================
// Coroutine 类实现
// ============================================================================

Coroutine::Coroutine(const CoroutineFunc func, void* arg,
                     const size_t stack_size)
    : stack_size_(stack_size), func_(func), arg_(arg) {
  stack_ = std::make_unique<char[]>(stack_size);

  getcontext(&context_);
  context_.uc_stack.ss_sp = stack_.get();
  context_.uc_stack.ss_size = stack_size;
  context_.uc_link = nullptr;

  makecontext(&context_,
              reinterpret_cast<void (*)()>(Scheduler::wrapper_function), 1,
              this);
  state_ = CoroutineState::READY;
  waiting_fd_ = -1;
}

ucontext_t& Coroutine::context() { return context_; }

const ucontext_t& Coroutine::context() const { return context_; }

CoroutineState Coroutine::state() const { return state_; }

int Coroutine::waiting_fd() const { return waiting_fd_; }

void Coroutine::set_state(const CoroutineState state) { state_ = state; }

void Coroutine::set_waiting_fd(const int fd) { waiting_fd_ = fd; }

void Coroutine::clear_waiting_fd() { waiting_fd_ = -1; }

void Coroutine::execute() const { func_(arg_); }

// ============================================================================
// Scheduler 类实现
// ============================================================================

Scheduler::Scheduler() {
  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ < 0) {
    std::cerr << "错误: 无法创建epoll文件描述符: " << strerror(errno)
              << std::endl;
    std::cerr << "协程调度器初始化失败，程序退出" << std::endl;
    exit(1);
  }
  std::cout << "协程调度器初始化成功" << std::endl;
}

Scheduler::~Scheduler() {
  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
  }
  std::cout << "协程调度器析构完成" << std::endl;
}

Scheduler& Scheduler::instance() {
  if (!instance_) {
    instance_ = std::unique_ptr<Scheduler>(new Scheduler());
  }
  return *instance_;
}

Coroutine* Scheduler::create_coroutine(Coroutine::CoroutineFunc func,
                                       void* arg) {
  all_coroutines_.push_back(std::make_unique<Coroutine>(func, arg));
  Coroutine* co_ptr = all_coroutines_.back().get();
  ready_queue_.push(co_ptr);
  return co_ptr;
}

void Scheduler::yield() const {
  if (!current_coroutine_) {
    std::cerr << "警告：不在协程中调用yield" << std::endl;
    return;
  }
  current_coroutine_->set_state(CoroutineState::READY);
  swapcontext(&current_coroutine_->context(), &main_context_);
}

void Scheduler::wait_for_read(const int fd) {
  if (!current_coroutine_) {
    std::cerr << "警告：不在协程中调用wait_for_read" << std::endl;
    return;
  }

  if (current_coroutine_->waiting_fd() != -1) {
    std::cerr << "警告：协程已在等待fd " << current_coroutine_->waiting_fd()
              << std::endl;
    return;
  }

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;

  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
    if (errno == EEXIST) {
      epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    } else {
      perror("epoll_ctl failed in wait_for_read");
      return;
    }
  }

  current_coroutine_->set_state(CoroutineState::WAITING);
  current_coroutine_->set_waiting_fd(fd);
  fd_to_coroutine_[fd] = current_coroutine_;

  swapcontext(&current_coroutine_->context(), &main_context_);
}
bool Scheduler::in_coroutine() const { return current_coroutine_ != nullptr; }

bool Scheduler::has_waiting_coroutines() const {
  return !fd_to_coroutine_.empty();
}

ucontext_t& Scheduler::get_main_context() { return main_context_; }

void Scheduler::wrapper_function(Coroutine* coroutine) {
  try {
    coroutine->execute();
  } catch (const std::exception& e) {
    std::cerr << "协程异常: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "协程未知异常" << std::endl;
  }
  coroutine->set_state(CoroutineState::FINISHED);

  Scheduler& scheduler = instance();
  swapcontext(&coroutine->context(), &scheduler.get_main_context());
}

void Scheduler::check_io_events() {
  epoll_event events[64]{};
  const int nfds = epoll_wait(epoll_fd_, events, 64, 100);

  if (nfds < 0) {
    if (errno != EINTR) {
      perror("epoll_wait failed");
    }
    return;
  }

  // 只在有事件时才打印
  if (nfds > 0) {
#ifdef DEBUG
    std::cout << "检测到 " << nfds << " 个IO事件" << std::endl;
#endif
  }

  for (int i{0}; i < nfds; ++i) {
    int ready_fd = events[i].data.fd;

    if (Coroutine* waiting_co = fd_to_coroutine_[ready_fd];
        waiting_co && waiting_co->state() == CoroutineState::WAITING) {
#ifdef DEBUG
      std::cout << "唤醒fd " << ready_fd << " 的协程" << std::endl;
#endif
      waiting_co->set_state(CoroutineState::READY);
      waiting_co->clear_waiting_fd();
      fd_to_coroutine_.erase(ready_fd);
      epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, ready_fd, nullptr);
      ready_queue_.push(waiting_co);
    }
  }
}

void Scheduler::handle_coroutine_return() {
  if (!current_coroutine_) {
    return;
  }

  switch (current_coroutine_->state()) {
    case CoroutineState::READY:
      ready_queue_.push(current_coroutine_);
      break;

    case CoroutineState::WAITING:
    case CoroutineState::FINISHED:
      break;
  }
}

void Scheduler::run_one_coroutine() {
  if (ready_queue_.empty()) {
    return;
  }

  Coroutine* next = ready_queue_.front();
  ready_queue_.pop();
  current_coroutine_ = next;

  // 切换到协程上下文
  swapcontext(&main_context_, &next->context());

  // 处理协程返回后的状态
  if (current_coroutine_) {
    handle_coroutine_return();
  }

  current_coroutine_ = nullptr;
}

void Scheduler::schedule_once() {
  if (!main_context_ready_) {
    getcontext(&main_context_);
    main_context_ready_ = true;
    std::cout << "协程调度器就绪" << std::endl;
  }

  // 高负载时减少清理频率
  if (const int cleanup_interval =
          (all_coroutines_.size() > 100000) ? 10000 : 1000;
      ++schedule_count_ >= cleanup_interval) {
    cleanup_finished_coroutines();
    schedule_count_ = 0;
  }

  if (!ready_queue_.empty()) {
    run_one_coroutine();
    reset_idle_state();
    return;
  }

  if (has_waiting_coroutines()) {
    check_io_events();
    reset_idle_state();
    return;
  }

  handle_idle_state();
}

void Scheduler::handle_idle_state() {
  const auto now = std::chrono::steady_clock::now();

  if (!is_idle_) {
    is_idle_ = true;
    idle_start_time_ = now;
    return;  // 第一次进入空闲时只打印一次消息就返回
  }

  // 空闲状态下不需要频繁打印任何消息
  // 如果真的需要周期性状态报告，可以设置很长的间隔
  const auto idle_duration =
      std::chrono::duration_cast<std::chrono::minutes>(now - idle_start_time_)
          .count();

  // 每10分钟打印一次状态（可选）
  if (idle_duration > 0 && idle_duration % 10 == 0) {
    static long last_printed_minutes = 0;
    if (idle_duration != last_printed_minutes) {
      std::cout << "服务器运行正常，已运行 " << idle_duration << " 分钟"
                << std::endl;
      last_printed_minutes = idle_duration;
    }
  }
}

void Scheduler::cleanup_finished_coroutines() {
  const size_t original_size = all_coroutines_.size();

  auto it = all_coroutines_.begin();
  while (it != all_coroutines_.end()) {
    if ((*it)->state() == CoroutineState::FINISHED) {
      it = all_coroutines_.erase(it);
    } else {
      ++it;
    }
  }

  const size_t cleaned_count = original_size - all_coroutines_.size();
  if (cleaned_count > 0) {
    std::cout << "清理了 " << cleaned_count << " 个已完成的协程，"
              << "剩余 " << all_coroutines_.size() << " 个协程" << std::endl;
  }
}

void Scheduler::reset_idle_state() { is_idle_ = false; }