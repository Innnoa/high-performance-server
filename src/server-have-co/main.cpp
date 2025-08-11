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
std::atomic<bool> shutdown_in_progress{false};

void signal_handler(int signal) {
  // 防止重复处理信号
  if (shutdown_in_progress.exchange(true)) {
    Logger::info("Shutdown already in progress, ignoring signal");
    return;
  }

  switch (signal) {
    case SIGINT:
      Logger::info("Received SIGINT (Ctrl+C), initiating graceful shutdown...");
      break;
    case SIGTERM:
      Logger::info("Received SIGTERM, initiating graceful shutdown...");
      break;
    default:
      Logger::info("Received signal " + std::to_string(signal) +
                   ", initiating graceful shutdown...");
      break;
  }

  if (g_server) {
    Logger::info("Stopping server and cleaning up resources...");
    g_server->stop();
  }
}

void setup_signal_handlers() {
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;  // 添加SA_RESTART标志

  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  Logger::info("Signal handlers configured successfully");
}

bool check_system_requirements() {
  Logger::info("Checking system requirements...");

  // Check ucontext support
  ucontext_t test_context;
  if (getcontext(&test_context) != 0) {
    Logger::error("System does not support ucontext - coroutines unavailable");
    return false;
  }
  Logger::info("ucontext support verified");

  // Check file descriptor limits
  rlimit rlim{};
  if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
    Logger::info("File descriptor limits:");
    Logger::info("  Current: " + std::to_string(rlim.rlim_cur));
    Logger::info("  Maximum: " + std::to_string(rlim.rlim_max));

    if (rlim.rlim_cur < 10000) {
      Logger::info("Recommendation: ulimit -n 65536 (for better performance)");
    } else if (rlim.rlim_cur >= 65536) {
      Logger::info("Excellent fd limit configuration");
    } else {
      Logger::info("Good fd limit configuration");
    }
  }

  Logger::info("System requirements check completed successfully");
  return true;
}

void print_startup_banner() {
  std::cout << "\n";
  std::cout
      << "================================================================\n";
  std::cout
      << "         High-Performance Mixed IO Server v2.1                 \n";
  std::cout
      << "================================================================\n";
  std::cout << "Features:\n";
  std::cout << "  • Smart Mixed IO (100K Coroutines + Unlimited Normal IO)\n";
  std::cout << "  • RAII Zero-Leak Memory Management\n";
  std::cout << "  • Million-Level Concurrent Connections\n";
  std::cout << "  • 8KB Coroutine Stack + Hook Transparent Switching\n";
  std::cout << "  • Dynamic Container-Based Connection Management\n";
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
    for (uint16_t port : failed_ports) {
      Logger::error("  Port " + std::to_string(port));
    }
  }
}

void print_server_ready_message(size_t port_count) {
  std::cout << "\n" << std::string(60, '=') << std::endl;
  Logger::info("Server startup completed successfully");
  Logger::info("Listening on " + std::to_string(port_count) + " port(s)");
  Logger::info("Mixed IO mode active (adaptive connection handling)");
  Logger::info("Server is ready to accept connections...");
  Logger::info("Press Ctrl+C to initiate graceful shutdown");
  std::cout << std::string(60, '=') << std::endl;
}

void print_shutdown_summary() {
  std::cout << "\n" << std::string(50, '-') << std::endl;
  Logger::info("Server shutdown summary:");
  Logger::info("  All connections closed gracefully");
  Logger::info("  Memory pools cleaned up");
  Logger::info("  All resources released");
  Logger::info("Thank you for using High-Performance Mixed IO Server");
  std::cout << std::string(50, '-') << std::endl;
}

void safe_server_cleanup() {
  if (g_server) {
    Logger::info("Cleaning up server resources...");
    try {
      // 给服务器一些时间完成清理
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      g_server.reset();
      Logger::info("Server cleanup completed");
    } catch (const std::exception& e) {
      Logger::error("Exception during server cleanup: " +
                    std::string(e.what()));
    } catch (...) {
      Logger::error("Unknown exception during server cleanup");
    }
  }
}

int main() {
  try {
    print_startup_banner();

    // System checks
    if (!check_system_requirements()) {
      Logger::error("System requirements not met - exiting");
      return 1;
    }

    // Signal handling
    setup_signal_handlers();

    // Server creation
    Logger::info("Creating server instance...");
    g_server = std::make_unique<LeakSafeHighPerfServer>();
    Logger::info("Server instance created successfully");

    // Port configuration
    Logger::info("Configuring listening ports...");
    std::vector<uint16_t> successful_ports;
    std::vector<uint16_t> failed_ports;

    for (uint16_t port = 2048; port < 2068; ++port) {
      try {
        g_server->add_listen_port(port);
        successful_ports.push_back(port);
      } catch (const std::exception& e) {
        failed_ports.push_back(port);
        if (successful_ports.empty() && port < 2058) {
          // 只在前10个端口失败时记录错误，避免日志过多
          Logger::error("Failed to bind port " + std::to_string(port) + ": " +
                        e.what());
        }
      }
    }

    // Check if we have at least one working port
    if (successful_ports.empty()) {
      Logger::error("No ports could be bound - server cannot start");
      Logger::error("Please check:");
      Logger::error("  Port availability: netstat -tlnp | grep :204");
      Logger::error("  Process permissions");
      Logger::error("  System resource limits");
      return 1;
    }

    print_port_configuration_summary(successful_ports, failed_ports);
    print_server_ready_message(successful_ports.size());

    // Start server
    Logger::info("Starting main event loop...");
    g_server->run();

    // Graceful shutdown
    Logger::info("Main event loop terminated");
    safe_server_cleanup();

    print_shutdown_summary();

  } catch (const std::exception& e) {
    Logger::error("Server startup failed: " + std::string(e.what()));
    Logger::error("Troubleshooting checklist:");
    Logger::error("  Check port availability: netstat -tlnp | grep :2048");
    Logger::error("  Verify process permissions (try with sudo if needed)");
    Logger::error("  Check system resource limits: ulimit -a");
    Logger::error("  Ensure sufficient memory is available");
    Logger::error("  Check for conflicting processes");

    // 确保服务器被清理
    safe_server_cleanup();
    return 1;
  } catch (...) {
    Logger::error("Server encountered unknown critical error");
    Logger::error("This may indicate a serious system issue");

    // 确保服务器被清理
    safe_server_cleanup();
    return 1;
  }

  return 0;
}