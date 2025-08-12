#include <sys/resource.h>
#include <ucontext.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

#include "logger.h"
#include "server.h"

std::unique_ptr<LeakSafeHighPerfServer> g_server = nullptr;
std::atomic shutdown_in_progress{false};

void signal_handler(const int signal) {
  if (shutdown_in_progress.exchange(true)) {
    Logger::info("正在关机, 忽略信号...");
    return;
  }

  switch (signal) {
    case SIGINT:
      Logger::info("收到SIGINT (Ctrl+C)，正常关机…");
      break;
    case SIGTERM:
      Logger::info("收到SIGTERM，正常关机…");
      break;
    default:
      Logger::info("接收到信号: " + std::to_string(signal) + ", 关机...");
      break;
  }

  if (g_server) {
    Logger::info("停止服务并清理资源...");
    g_server->stop();
  }
}

void setup_signal_handlers() {
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;

  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  Logger::info("成功配置信号处理程序");
}

bool check_system_requirements() {
  Logger::info("检查系统配置...");
  ucontext_t test_context;
  if (getcontext(&test_context) != 0) {
    Logger::error("系统不支持ucontest, 协程不可用");
    return false;
  }
  Logger::info("ucontest支持完全");

  rlimit rlim{};
  if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
    Logger::info("文件描述符限制");
    Logger::info("软限制(当前): " + std::to_string(rlim.rlim_cur));
    Logger::info("硬限制: " + std::to_string(rlim.rlim_max));

    if (rlim.rlim_cur < 1000000) {
      Logger::info("文件描述符预分配不足, 使用命令: ulimit -n 1048576");
    } else {
      Logger::info("文件描述符配置正常");
    }
  }

  Logger::info("系统配置检查完成");
  return true;
}

void print_startup_banner() {
  std::cout << "\n";
  std::cout
      << "================================================================\n";
  std::cout
      << "                C++高性能服务器2.1.0 - 协程版本                 \n";
  std::cout
      << "================================================================\n";
  std::cout << "简要:\n";
  std::cout << "• 智能混合IO (100K 协程 + 900K正常 IO)\n";
  std::cout << "• RAII内存管理\n";
  std::cout << "• 百万并发连接\n";
  std::cout << "• 8KB 协程栈 + Hook IO转换\n";
  std::cout << "• 动态容器分配连接\n";
  std::cout
      << "================================================================\n";
  std::cout << std::endl;
}

void print_port_configuration_summary(
    const std::vector<uint16_t>& successful_ports,
    const std::vector<uint16_t>& failed_ports) {
  if (!successful_ports.empty()) {
    Logger::info("Successfully configured listening ports:");
    for (size_t i = 0; i < successful_ports.size(); ++i) {
      std::string port_info = "  Port " + std::to_string(successful_ports[i]);
      if (i == 0) port_info += " (primary)";
      Logger::info(port_info);
    }
  }

  if (!failed_ports.empty()) {
    Logger::error("Failed to bind ports:");
    for (const uint16_t port : failed_ports) {
      Logger::error("  Port " + std::to_string(port));
    }
  }
}

void print_server_ready_message(const size_t port_count) {
  std::cout << "\n" << std::string(60, '=') << std::endl;
  Logger::info("服务器启动成功");
  Logger::info("监听了" + std::to_string(port_count) + "个端口(s)");
  Logger::info("混合IO模式 (自适应连接)");
  Logger::info("服务器待连接...");
  Logger::info("按 Ctrl+C 关机");
  std::cout << std::string(60, '=') << std::endl;
}

void print_shutdown_summary() {
  std::cout << "\n" << std::string(50, '-') << std::endl;
  Logger::info("服务器关闭摘要::");
  Logger::info("所有连接正常关闭");
  Logger::info("内存池清理完成");
  Logger::info("所有资源已释放");
  std::cout << std::string(50, '-') << std::endl;
}

void safe_server_cleanup() {
  if (g_server) {
    Logger::info("清理服务器资源...");
    try {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      g_server.reset();
      Logger::info("服务器清理完成");
    } catch (const std::exception& e) {
      Logger::error("服务器清理异常: " + std::string(e.what()));
    } catch (...) {
      Logger::error("服务器清理过程中抛出未知异常");
    }
  }
}

int main() {
  try {
    print_startup_banner();

    if (!check_system_requirements()) {
      Logger::error("系统未满足基本配置, 正在退出...");
      return 1;
    }

    setup_signal_handlers();

    Logger::info("创建服务器实例...");
    g_server = std::make_unique<LeakSafeHighPerfServer>();
    Logger::info("服务器实例创建成功");

    Logger::info("配置监听端口...");
    std::vector<uint16_t> successful_ports;
    std::vector<uint16_t> failed_ports;

    for (uint16_t port = 2048; port < 2068; ++port) {
      try {
        g_server->add_listen_port(port);
        successful_ports.push_back(port);
      } catch (const std::exception& e) {
        failed_ports.push_back(port);
        if (successful_ports.empty() && port < 2058) {
          Logger::error("绑定失败端口: " + std::to_string(port) + ": " +
                        e.what());
        }
      }
    }

    if (successful_ports.empty()) {
      Logger::error("没有端口被绑定,服务器启动失败");
      Logger::error("请检查:");
      Logger::error("端口可用性: netstat -tlnp | grep :204");
      Logger::error("程序权限");
      Logger::error("系统资源限制");
      return 1;
    }

    print_server_ready_message(successful_ports.size());

    Logger::info("启动回环加载...");
    g_server->run();

    Logger::info("回环终止");
    safe_server_cleanup();

    print_shutdown_summary();

  } catch (const std::exception& e) {
    Logger::error("服务器启动失败: " + std::string(e.what()));
    Logger::error("故障排除清单:");
    Logger::error("检查端口开放情况: netstat -tlnp | grep :2048");
    Logger::error("检查程序权限 (必要时可以使用 sudo)");
    Logger::error("检查系统资源限制: ulimit -a");
    Logger::error("确保有足够的内存可以使用");
    Logger::error("检查冲突的进程");

    safe_server_cleanup();
    return 1;
  } catch (...) {
    Logger::error("服务器遇到未知的严重错误");
    Logger::error("这可能表明存在严重的系统问题");

    safe_server_cleanup();
    return 1;
  }

  return 0;
}