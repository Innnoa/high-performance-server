#ifndef INC_2_2_1_1_COROUTINE_H
#define INC_2_2_1_1_COROUTINE_H

#endif  // INC_2_2_1_1_COROUTINE_H

#ifndef COROUTINE_H
#define COROUTINE_H

#include <ucontext.h>

#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <queue>

enum class CoroutineState { READY, WAITING, FINISHED };

class Coroutine {
 public:
  using CoroutineFunc = void (*)(void*);

  Coroutine(CoroutineFunc func, void* arg, size_t stack_size = 8192);

  Coroutine(const Coroutine&) = delete;
  Coroutine& operator=(const Coroutine&) = delete;
  Coroutine(Coroutine&&) = default;
  Coroutine& operator=(Coroutine&&) = default;

  ~Coroutine() = default;

  ucontext_t& context();
  [[nodiscard]] const ucontext_t& context() const;
  [[nodiscard]] CoroutineState state() const;
  [[nodiscard]] int waiting_fd() const;

  void set_state(CoroutineState state);
  void set_waiting_fd(int fd);
  void clear_waiting_fd();
  void execute() const;

 private:
  ucontext_t context_{};
  CoroutineState state_ = CoroutineState::READY;
  std::unique_ptr<char[]> stack_;
  size_t stack_size_;
  int waiting_fd_ = -1;
  CoroutineFunc func_;
  void* arg_;
};

class Scheduler {
 public:
  static Scheduler& instance();

  void schedule_once();
  Coroutine* create_coroutine(Coroutine::CoroutineFunc func, void* arg);
  void yield() const;
  void wait_for_read(int fd);

  [[nodiscard]] bool in_coroutine() const;
  [[nodiscard]] bool has_waiting_coroutines() const;

  ucontext_t& get_main_context();

  static void wrapper_function(Coroutine* coroutine);

  ~Scheduler();

 private:
  Scheduler();

  void check_io_events();
  void run_one_coroutine();
  void handle_coroutine_return();
  void handle_idle_state();
  void cleanup_finished_coroutines();
  void reset_idle_state();

  static thread_local std::unique_ptr<Scheduler> instance_;

  std::list<std::unique_ptr<Coroutine>> all_coroutines_;
  std::queue<Coroutine*> ready_queue_;
  Coroutine* current_coroutine_ = nullptr;
  ucontext_t main_context_{};
  int epoll_fd_ = -1;
  std::map<int, Coroutine*> fd_to_coroutine_;

  std::chrono::steady_clock::time_point idle_start_time_;
  bool is_idle_ = false;
  int schedule_count_ = 0;
  bool main_context_ready_ = false;
};

class HookSystem {
 public:
  static void initialize();
  static bool is_initialized();

 private:
  static bool initialized_;
};

#endif  // COROUTINE_H