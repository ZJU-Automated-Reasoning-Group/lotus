/*
 * Thread-Safe Data Structures
 *
 * This header provides generic thread-safe container wrappers for concurrent
 * programming. These data structures use fine-grained locking to ensure
 * thread safety while maintaining good performance in multi-threaded contexts.
 */

#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus {

// ============================================================================
// Simple Optional Type for C++14 Compatibility
// ============================================================================

template <typename T> class SimpleOptional {
private:
  T *m_value;
  bool m_has_value;

public:
  SimpleOptional() : m_value(nullptr), m_has_value(false) {}
  SimpleOptional(const T &value) : m_value(new T(value)), m_has_value(true) {}
  SimpleOptional(const SimpleOptional &other) : m_has_value(other.m_has_value) {
    if (m_has_value)
      m_value = new T(*other.m_value);
    else
      m_value = nullptr;
  }
  SimpleOptional &operator=(const SimpleOptional &other) {
    if (this != &other) {
      T *new_value = other.m_has_value ? new T(*other.m_value) : nullptr;
      if (m_value)
        delete m_value;
      m_value = new_value;
      m_has_value = other.m_has_value;
    }
    return *this;
  }
  ~SimpleOptional() {
    if (m_value)
      delete m_value;
  }
  bool has_value() const { return m_has_value; }
  const T &value() const { return *m_value; }
  T &value() { return *m_value; }
  explicit operator bool() const { return m_has_value; }
};

// ============================================================================
// Thread-Safe Set
// ============================================================================

template <typename T> class ThreadSafeSet {
private:
  mutable std::mutex m_mutex;
  std::unordered_set<T> m_set;

public:
  bool insert(const T &value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_set.insert(value).second;
  }

  bool erase(const T &value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_set.erase(value) != 0;
  }

  bool contains(const T &value) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_set.find(value) != m_set.end();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_set.size();
  }

  void clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_set.clear();
  }

  std::vector<T> snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<T>(m_set.begin(), m_set.end());
  }

  // For iteration (read-only operations). The callback executes while the set
  // remains locked.
  template <typename Func> void for_each(Func func) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &item : m_set) {
      func(item);
    }
  }
};

// ============================================================================
// Thread-Safe Map
// ============================================================================

template <typename K, typename V> class ThreadSafeMap {
private:
  mutable std::mutex m_mutex;
  std::unordered_map<K, V> m_map;

public:
  bool insert_or_assign(const K &key, const V &value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_map.find(key);
    if (it != m_map.end()) {
      it->second = value;
      return false; // Updated existing
    } else {
      m_map[key] = value;
      return true; // Inserted new
    }
  }

  bool erase(const K &key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_map.erase(key) != 0;
  }

  SimpleOptional<V> get(const K &key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_map.find(key);
    return it != m_map.end() ? SimpleOptional<V>(it->second)
                             : SimpleOptional<V>();
  }

  bool contains(const K &key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_map.find(key) != m_map.end();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_map.size();
  }

  void clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_map.clear();
  }

  std::unordered_map<K, V> snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_map;
  }

  template <typename Func> bool update(const K &key, Func func) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_map.find(key);
    if (it == m_map.end())
      return false;
    func(it->second);
    return true;
  }

  // For iteration (read-only operations). The callback executes while the map
  // remains locked.
  template <typename Func> void for_each(Func func) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &item : m_map) {
      func(item);
    }
  }

  // Union operation for set-valued maps
  // Returns true if any new element was added
  template <typename T> bool union_with(const K &key, const T &new_element) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_map.find(key);
    if (it != m_map.end()) {
      // Key exists - perform set union
      auto result = it->second.insert(new_element);
      return result.second; // true if element was newly inserted
    } else {
      // Key doesn't exist - create new set with element
      V new_set;
      new_set.insert(new_element);
      m_map[key] = new_set;
      return true;
    }
  }
};

// ============================================================================
// Thread-Safe Vector
// ============================================================================

template <typename T> class ThreadSafeVector {
private:
  mutable std::mutex m_mutex;
  std::vector<T> m_vector;

public:
  void push_back(const T &value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vector.push_back(value);
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_vector.size();
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_vector.empty();
  }

  SimpleOptional<T> pop_back() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_vector.empty()) {
      return SimpleOptional<T>();
    }
    T value = m_vector.back();
    m_vector.pop_back();
    return SimpleOptional<T>(value);
  }

  // Bulk pop for better performance
  std::vector<T> pop_batch(size_t max_count) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<T> batch;
    size_t count = std::min(max_count, m_vector.size());
    if (count > 0) {
      batch.reserve(count);
      auto begin_it = m_vector.end() - count;
      batch.insert(batch.end(), begin_it, m_vector.end());
      m_vector.erase(begin_it, m_vector.end());
    }
    return batch;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vector.clear();
  }

  std::vector<T> snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_vector;
  }
};

// ============================================================================
// Sharded Map for Reduced Lock Contention
// ============================================================================

// Sharded map uses multiple independent locks to reduce contention
// in highly concurrent scenarios. The template parameter NumShards
// controls the number of shards (default: 64).
template <typename K, typename V, size_t NumShards = 64> class ShardedMap {
private:
  struct Shard {
    mutable std::mutex mutex;
    std::unordered_map<K, V> map;
  };

  std::array<Shard, NumShards> m_shards;

  size_t get_shard_index(const K &key) const {
    return std::hash<K>{}(key) % NumShards;
  }

  Shard &get_shard(const K &key) { return m_shards[get_shard_index(key)]; }

  const Shard &get_shard(const K &key) const {
    return m_shards[get_shard_index(key)];
  }

public:
  bool insert_or_assign(const K &key, const V &value) {
    auto &shard = get_shard(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(key);
    if (it != shard.map.end()) {
      it->second = value;
      return false;
    } else {
      shard.map[key] = value;
      return true;
    }
  }

  SimpleOptional<V> get(const K &key) const {
    const auto &shard = get_shard(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(key);
    return it != shard.map.end() ? SimpleOptional<V>(it->second)
                                 : SimpleOptional<V>();
  }

  bool contains(const K &key) const {
    const auto &shard = get_shard(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.map.find(key) != shard.map.end();
  }

  size_t size() const {
    size_t total = 0;
    for (const auto &shard : m_shards) {
      std::lock_guard<std::mutex> lock(shard.mutex);
      total += shard.map.size();
    }
    return total;
  }

  void clear() {
    for (auto &shard : m_shards) {
      std::lock_guard<std::mutex> lock(shard.mutex);
      shard.map.clear();
    }
  }

  template <typename Func> void for_each(Func func) const {
    for (const auto &shard : m_shards) {
      std::lock_guard<std::mutex> lock(shard.mutex);
      for (const auto &item : shard.map) {
        func(item);
      }
    }
  }

  // Union operation for set-valued maps
  template <typename T> bool union_with(const K &key, const T &new_element) {
    auto &shard = get_shard(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(key);
    if (it != shard.map.end()) {
      auto result = it->second.insert(new_element);
      return result.second;
    } else {
      V new_set;
      new_set.insert(new_element);
      shard.map[key] = new_set;
      return true;
    }
  }
};

} // namespace lotus
