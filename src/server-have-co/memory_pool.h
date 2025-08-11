#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

// 缓冲区内存池 - 添加线程安全
class BufferPool {
 private:
  struct BufferChunk {
    std::unique_ptr<char[]> data;
    size_t size;

    BufferChunk(size_t count, size_t buffer_size)
        : data(std::make_unique<char[]>(count * buffer_size)),
          size(count * buffer_size) {}
  };

  std::vector<BufferChunk> buffer_chunks_;
  std::queue<char*> free_buffers_;
  size_t chunk_size_;
  size_t buffer_size_;
  mutable std::mutex mutex_;            // 添加互斥锁
  std::atomic<bool> destroyed_{false};  // 标记是否已销毁

 public:
  BufferPool(size_t buffer_size, size_t initial_count)
      : chunk_size_(initial_count), buffer_size_(buffer_size) {
    allocate_chunk();
  }

  ~BufferPool() {
    destroyed_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    // 清空队列，但不需要手动释放内存（unique_ptr会处理）
    while (!free_buffers_.empty()) {
      free_buffers_.pop();
    }
  }

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;
  BufferPool(BufferPool&&) = delete;  // 禁用移动，避免复杂的状态管理
  BufferPool& operator=(BufferPool&&) = delete;

  char* get_buffer() {
    if (destroyed_.load()) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    if (destroyed_.load()) return nullptr;

    if (free_buffers_.empty()) {
      allocate_chunk();
    }
    if (free_buffers_.empty()) return nullptr;

    char* buffer = free_buffers_.front();
    free_buffers_.pop();
    return buffer;
  }

  void return_buffer(char* buffer) {
    if (!buffer || destroyed_.load()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (destroyed_.load()) return;

    if (is_valid_buffer(buffer)) {
      free_buffers_.push(buffer);
    }
  }

  [[nodiscard]] size_t get_total_allocated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_chunks_.size() * chunk_size_ * buffer_size_;
  }

 private:
  void allocate_chunk() {
    try {
      buffer_chunks_.emplace_back(chunk_size_, buffer_size_);
      const auto& chunk = buffer_chunks_.back();
      char* base = chunk.data.get();
      for (size_t i = 0; i < chunk_size_; ++i) {
        free_buffers_.push(base + i * buffer_size_);
      }
    } catch (const std::bad_alloc&) {
      std::cerr << "Failed to allocate buffer chunk" << std::endl;
    }
  }

  bool is_valid_buffer(const char* buffer) const {
    for (const auto& chunk : buffer_chunks_) {
      const char* start = chunk.data.get();
      const char* end = start + chunk.size;
      if (buffer >= start && buffer < end) {
        return (buffer - start) % buffer_size_ == 0;
      }
    }
    return false;
  }
};

// 连接对象内存池 - 添加线程安全和更安全的reset
template <typename T>
class ObjectPool {
 private:
  struct ObjectChunk {
    std::unique_ptr<T[]> data;
    size_t count;

    explicit ObjectChunk(size_t chunk_count)
        : data(std::make_unique<T[]>(chunk_count)), count(chunk_count) {}
  };

  std::vector<ObjectChunk> object_chunks_;
  std::queue<T*> free_objects_;
  size_t chunk_size_;
  mutable std::mutex mutex_;
  std::atomic<bool> destroyed_{false};

 public:
  explicit ObjectPool(size_t initial_count) : chunk_size_(initial_count) {
    allocate_chunk();
  }

  ~ObjectPool() {
    destroyed_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    while (!free_objects_.empty()) {
      free_objects_.pop();
    }
  }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) = delete;
  ObjectPool& operator=(ObjectPool&&) = delete;

  T* get_object() {
    if (destroyed_.load()) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    if (destroyed_.load()) return nullptr;

    if (free_objects_.empty()) {
      allocate_chunk();
    }
    if (free_objects_.empty()) return nullptr;

    T* obj = free_objects_.front();
    free_objects_.pop();
    return obj;
  }

  void return_object(T* obj) {
    if (!obj || destroyed_.load()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (destroyed_.load()) return;

    if (is_valid_object(obj)) {
      // 安全地重置对象状态
      try {
        // 先手动清理可能的指针成员
        reset_object_safely(obj);
        // 然后placement new重新初始化
        new (obj) T();
        free_objects_.push(obj);
      } catch (...) {
        // 如果重置失败，不放回池中，避免污染
        std::cerr << "Failed to reset object, not returning to pool"
                  << std::endl;
      }
    }
  }

  [[nodiscard]] size_t get_total_allocated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return object_chunks_.size() * chunk_size_;
  }

 private:
  void allocate_chunk() {
    try {
      object_chunks_.emplace_back(chunk_size_);
      const auto& chunk = object_chunks_.back();
      T* base = chunk.data.get();
      for (size_t i = 0; i < chunk_size_; ++i) {
        free_objects_.push(&base[i]);
      }
    } catch (const std::bad_alloc&) {
      std::cerr << "Failed to allocate object chunk" << std::endl;
    }
  }

  bool is_valid_object(const T* obj) const {
    for (const auto& chunk : object_chunks_) {
      const T* start = chunk.data.get();
      const T* end = start + chunk.count;
      if (obj >= start && obj < end) {
        return true;
      }
    }
    return false;
  }

  // 安全重置对象 - 特化处理Connection类型
  void reset_object_safely(T* obj) {
    // 对于Connection类型，手动清理指针
    if constexpr (std::is_same_v<T, struct Connection>) {
      obj->fd = -1;
      obj->read_buffer = nullptr;
      obj->write_buffer = nullptr;
      obj->coroutine = nullptr;
      obj->server = nullptr;
      obj->coroutine_active = false;
      obj->last_activity = 0;
      obj->read_pos = 0;
      obj->write_pos = 0;
    }
  }
};

#endif  // MEMORY_POOL_H