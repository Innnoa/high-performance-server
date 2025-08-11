# high-performance-server

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.linux.org/)

一个能处理超过100万并发连接的高性能C++服务器，基于Linux epoll实现。

## 项目背景

做这个项目是为了锻炼在linux环境下开发的基本能力,同时想看看用单线程epoll配合仔细的内存管理和系统调优能推到什么程度。

经过几轮迭代和大量性能分析，最终稳定跑到了100万+连接，内存使用也还算合理。

## 实测性能

以下是我实际测试和测量的结果：

| 指标 | 结果 | 说明 |
|------|------|------|
| **最大连接数** | 1,022,106 | Ubuntu 20.04，16GB内存环境 |
| **内存使用** | ~557MB | 100万+连接时 |
| **连接建立速率** | ~1,400/秒 | 爬坡阶段 |
| **成功率** | 99.9% | 受控环境下 |
| **单连接内存** | ~0.54KB | 包含所有开销 |

**重要说明**：这些数据来自专用测试环境。你的结果会因硬件、内核版本和系统配置而有所不同。

## 工作原理

核心架构:

- **事件循环**：单线程epoll，边缘触发模式
- **内存池**：预分配缓冲区，避免运行时malloc/free
- **连接管理**：自定义分配器配合对象池
- **缓冲区管理**：小的固定大小缓冲区（256字节），重复使用

专注于：
1. 运行时最小化内存分配
2. 尽可能降低单连接开销  
3. 在可能的地方批量处理系统调用

## 快速开始

**环境要求：**
- Linux系统（在Ubuntu 18.04+上测试过）
- GCC 11.0+ 或 Clang 13.0+
- 至少4GB内存用于测试

**系统调优（重要）：**
```bash
# 必须要做，否则会在1024连接左右碰壁
ulimit -n 1048576
echo "net.core.somaxconn = 65535" | sudo tee -a /etc/sysctl.conf
sudo sysctl -p
```

**clone code：**
```bash
git clone https://github.com/Innnoa/high-performance-server.git
cd high-performance-server
```

**编译运行 server ：**
```bash
cd src/server
mkdir build && cd build
cmake ..
make
./server
```

**编译运行 client ：**
```bash
cd src/client
mkdir build && cd build
cmake ..
make
./client [server端ip/127.0.0.1] 2048
```

## 项目结构

```
src/
├── server/
│   ├── main.cpp                 # 主服务器实现
├── client/
│   └── client.cpp               # 测试客户端
│── docs/                        # 文档和指南
│── .git/
│── CONTRIBUTING
│── LICENSE
│── README      
```

## 实际使用考虑

**适用场景：**
- 大量连接数，低内存使用
- 可预测的性能特征
- 适合很多空闲连接的场景

**当前限制：**
- 单线程（高吞吐量时CPU受限）
- 仅限Linux（使用epoll）
- 简单回显服务器（需要自己添加协议）
- 没有内置负载均衡

**已知问题：**
- 快速建连时内存使用可能激增
- 需要仔细调优系统参数
- 虚拟化环境下性能会下降

## 配置选项

大部分设置是编译时常量：

```cpp
static constexpr size_t MAX_CONNECTIONS = 1000000;
static constexpr size_t SMALL_BUFFER_SIZE = 256;
static constexpr size_t MAX_EVENTS = 8192;
```

针对不同用例，你可能需要调整：
- `SMALL_BUFFER_SIZE` - 如果需要更大消息就调大
- `MAX_EVENTS` - 更多并发I/O时调大
- `POOL_CHUNK_SIZE` - 影响内存分配模式

## 测试

基本功能测试：
```bash
make test
```

内存泄漏测试：
```bash
valgrind --leak-check=full ./server
```

压力测试：
```bash
./scripts/stress_test.sh
```

## 贡献

欢迎提交问题或PR。有几个可以改进的地方：

- 多线程支持
- 协议实现（HTTP、WebSocket等）
- Windows/macOS兼容性
- 更好的监控和指标
- 性能优化

## 技术细节

**内存管理：**
最大的挑战是保持单连接内存使用量低。用`std::vector`缓冲区的标准方法每连接要用8KB+。改成了：
- 固定大小缓冲区池
- 自定义连接分配器
- 仔细的结构体打包

**系统限制：**
达到100万连接前会遇到各种系统限制：
- 文件描述符限制（`ulimit -n`）
- Socket缓冲区内存（`net.core.rmem_max`）
- 连接跟踪（`net.netfilter.nf_conntrack_max`）

详细的系统配置见`docs/TUNING.md`。

**性能分析：**
开发过程中大量使用了`perf`。主要瓶颈是：
- 内存分配（用池解决了）
- 系统调用开销（用批处理解决了）
- 缓存未命中（用更好的数据布局解决了）

## 开源协议

MIT License - 详见LICENSE文件。

## 致谢

受到了各种高性能服务器项目的启发：
- nginx的事件处理
- Redis的内存管理技术
- libevent的设计模式

感谢Linux内核开发者让epoll这么好用。
