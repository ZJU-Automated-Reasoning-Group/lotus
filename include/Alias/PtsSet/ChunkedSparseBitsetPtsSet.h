// Chunked sparse bitset points-to set.
//
// Representation:
// - A sorted vector of fixed-size chunks.
// - Each chunk stores kChunkBits bits in kWords 64-bit words.
// - Operations are linear in the number of non-empty chunks.
//
// Not thread-safe: the iterator compares by position, not container identity.
// This file is standalone and not wired into any analysis yet.
#ifndef ANDERSEN_CHUNKED_SPARSE_BITSET_PTSSET_H
#define ANDERSEN_CHUNKED_SPARSE_BITSET_PTSSET_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <type_traits>

#include <llvm/ADT/SmallVector.h>

class ChunkedSparseBitsetPtsSet {
public:
  using Index = std::uint64_t;

  static constexpr std::size_t kChunkBits = 1024;
  static constexpr std::size_t kWordBits = 64;
  static constexpr std::size_t kWords = kChunkBits / kWordBits;

  static_assert(kChunkBits % kWordBits == 0,
                "Chunk size must be a multiple of word size.");

  // Hot words first for streaming; align to cache line (16 words = 128 B = 2
  // cache lines).
  struct alignas(64) Chunk {
    std::array<std::uint64_t, kWords> words{};

    bool operator==(const Chunk &other) const { return words == other.words; }
  };

  static_assert(
      std::is_trivially_copyable<Chunk>::value,
      "Chunk must be trivially copyable for memcpy-style optimisations.");

  static constexpr Chunk kZeroChunk{{}};

  using IdsVec = llvm::SmallVector<std::uint64_t, 2>;
  using ChunksVec = llvm::SmallVector<Chunk, 2>;

  class iterator {
  public:
    using value_type = Index;
    using difference_type = Index;
    using pointer = const Index *;
    using reference = const Index &;
    using iterator_category = std::forward_iterator_tag;

    // Default-constructed iterator is an end sentinel (STL-friendly).
    iterator() = default;

    Index operator*() const { return current_; }

    iterator &operator++() {
      advance();
      return *this;
    }

    bool operator==(const iterator &other) const {
      return end_ == other.end_ && (end_ || (chunk_pos_ == other.chunk_pos_ &&
                                             word_pos_ == other.word_pos_ &&
                                             cur_word_ == other.cur_word_));
    }

    bool operator!=(const iterator &other) const { return !(*this == other); }

  private:
    friend class ChunkedSparseBitsetPtsSet;

    explicit iterator(const IdsVec *ids, const ChunksVec *chunks, bool end)
        : ids_(ids), chunks_(chunks), end_(end) {
      if (ids_ && chunks_ && !chunks_->empty() && !end_)
        findFirst();
    }

    void findFirst() {
      for (chunk_pos_ = 0; chunk_pos_ < chunks_->size(); ++chunk_pos_) {
        const auto &chunk = (*chunks_)[chunk_pos_];
        for (word_pos_ = 0; word_pos_ < kWords; ++word_pos_) {
          cur_word_ = chunk.words[word_pos_];
          if (cur_word_ != 0) {
            setCurrent();
            return;
          }
        }
      }
      end_ = true;
    }

    void setCurrent() {
      current_ = baseAt() + static_cast<Index>(word_pos_ * kWordBits +
                                               countTrailingZeros(cur_word_));
    }

    Index baseAt() const {
      return static_cast<Index>((*ids_)[chunk_pos_]) * kChunkBits;
    }

    void advance() {
      if (end_)
        return;
      // cur_word_ != 0 here, so x & (x-1) is safe (no UB when x==0).
      cur_word_ &= cur_word_ - 1;
      while (cur_word_ == 0) {
        ++word_pos_;
        if (word_pos_ >= kWords) {
          ++chunk_pos_;
          word_pos_ = 0;
          if (chunk_pos_ >= chunks_->size()) {
            end_ = true;
            return;
          }
        }
        cur_word_ = (*chunks_)[chunk_pos_].words[word_pos_];
      }
      setCurrent();
    }

    static inline int countTrailingZeros(std::uint64_t value) {
#if defined(__clang__) || defined(__GNUC__)
      return value ? __builtin_ctzll(value) : 64;
#else
      if (value == 0)
        return 64;
      int n = 0;
      while ((value & 1) == 0) {
        value >>= 1;
        ++n;
      }
      return n;
#endif
    }

    const IdsVec *ids_ = nullptr;
    const ChunksVec *chunks_ = nullptr;
    std::size_t chunk_pos_ = 0;
    std::size_t word_pos_ = 0;
    std::uint64_t cur_word_ = 0;
    Index current_ = 0;
    bool end_ = true;
  };

  // Return true if *this contains idx.
  [[nodiscard]] bool contains(Index idx) const {
    const std::uint64_t chunk_id = chunkId(idx);
    const std::uint64_t offset = chunkOffset(idx);
    const std::size_t word = static_cast<std::size_t>(offset >> 6);
    const std::size_t bit = static_cast<std::size_t>(offset & 63);

    const std::size_t pos = findChunk(chunk_id);
    if (pos == ids_.size() || ids_[pos] != chunk_id)
      return false;
    return (chunks_[pos].words[word] >> bit) & 1U;
  }

  // Return true if the set changes.
  bool insert(Index idx) {
    const std::uint64_t chunk_id = chunkId(idx);
    const std::uint64_t offset = chunkOffset(idx);
    const std::size_t word = static_cast<std::size_t>(offset >> 6);
    const std::size_t bit = static_cast<std::size_t>(offset & 63);
    const std::uint64_t mask = std::uint64_t{1} << bit;

    // Fast path: append new chunk (monotonic insert).
    if (ids_.empty() || ids_.back() < chunk_id) {
      ids_.push_back(chunk_id);
      Chunk c = kZeroChunk;
      c.words[word] = mask;
      chunks_.push_back(c);
      ++size_;
      return true;
    }
    if (ids_.back() == chunk_id) {
      std::uint64_t &word_ref = chunks_.back().words[word];
      if (__builtin_expect(static_cast<bool>(word_ref & mask), 0))
        return false;
      word_ref |= mask;
      ++size_;
      return true;
    }

    const std::size_t pos = findChunk(chunk_id);
    if (pos == ids_.size() || ids_[pos] != chunk_id) {
      ids_.insert(ids_.begin() + static_cast<std::ptrdiff_t>(pos), chunk_id);
      Chunk c = kZeroChunk;
      c.words[word] = mask;
      chunks_.insert(chunks_.begin() + static_cast<std::ptrdiff_t>(pos), c);
      ++size_;
      return true;
    }

    std::uint64_t &word_ref = chunks_[pos].words[word];
    if (__builtin_expect(static_cast<bool>(word_ref & mask), 0))
      return false;
    word_ref |= mask;
    ++size_;
    return true;
  }

  [[nodiscard]] bool contains(const ChunkedSparseBitsetPtsSet &other) const {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < ids_.size() && j < other.ids_.size()) {
      if (ids_[i] < other.ids_[j]) {
        ++i;
        continue;
      }
      if (ids_[i] > other.ids_[j])
        return false;
      const auto &lhs = chunks_[i];
      const auto &rhs = other.chunks_[j];
      for (std::size_t w = 0; w < kWords; ++w) {
        if ((lhs.words[w] & rhs.words[w]) != rhs.words[w])
          return false;
      }
      ++i;
      ++j;
    }
    return j == other.ids_.size();
  }

  // Return true if this set and other have at least one element in common
  // (read-only).
  [[nodiscard]] bool intersects(const ChunkedSparseBitsetPtsSet &other) const {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < ids_.size() && j < other.ids_.size()) {
      if (ids_[i] < other.ids_[j]) {
        ++i;
        continue;
      }
      if (ids_[i] > other.ids_[j]) {
        ++j;
        continue;
      }
      const auto &lhs = chunks_[i];
      const auto &rhs = other.chunks_[j];
      for (std::size_t w = 0; w < kWords; ++w) {
        if ((lhs.words[w] & rhs.words[w]) != 0)
          return true;
      }
      ++i;
      ++j;
    }
    return false;
  }

  // Return true if the set changes.
  bool unionWith(const ChunkedSparseBitsetPtsSet &other) {
    if (other.isEmpty())
      return false;

    // Quick scan: avoid allocating merged_ids/merged when nothing changes.
    bool changed = false;
    {
      std::size_t i = 0, j = 0;
      while (i < ids_.size() && j < other.ids_.size()) {
        if (ids_[i] < other.ids_[j]) {
          ++i;
          continue;
        }
        if (other.ids_[j] < ids_[i]) {
          if (!changed && !chunkAllZero(other.chunks_[j]))
            changed = true;
          ++j;
          if (changed)
            break;
          continue;
        }
        for (std::size_t w = 0; w < kWords; ++w) {
          if ((chunks_[i].words[w] | other.chunks_[j].words[w]) !=
              chunks_[i].words[w]) {
            changed = true;
            break;
          }
        }
        if (changed)
          break;
        ++i;
        ++j;
      }
      if (!changed && j != other.ids_.size()) {
        for (; j < other.ids_.size(); ++j)
          if (!chunkAllZero(other.chunks_[j])) {
            changed = true;
            break;
          }
      }
      if (!changed)
        return false;
    }

    IdsVec merged_ids;
    ChunksVec merged;
    merged_ids.reserve(ids_.size() + other.ids_.size());
    merged.reserve(chunks_.size() + other.chunks_.size());
    std::size_t new_size = 0;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < ids_.size() || j < other.ids_.size()) {
      if (j == other.ids_.size() ||
          (i < ids_.size() && ids_[i] < other.ids_[j])) {
        const Chunk &c = chunks_[i++];
        if (!chunkAllZero(c)) {
          merged_ids.push_back(ids_[i - 1]);
          merged.push_back(c);
          new_size += chunkPopcount(c);
        }
        continue;
      }
      if (i == ids_.size() || other.ids_[j] < ids_[i]) {
        const Chunk &c = other.chunks_[j];
        if (!chunkAllZero(c)) {
          merged_ids.push_back(other.ids_[j]);
          merged.push_back(c);
          new_size += chunkPopcount(c);
        }
        ++j;
        continue;
      }
      Chunk out = chunks_[i];
      for (std::size_t w = 0; w < kWords; ++w)
        out.words[w] |= other.chunks_[j].words[w];
      if (!chunkAllZero(out)) {
        merged_ids.push_back(ids_[i]);
        merged.push_back(out);
        new_size += chunkPopcount(out);
      }
      ++i;
      ++j;
    }

    ids_.swap(merged_ids);
    chunks_.swap(merged);
    size_ = new_size;
    return true;
  }

  void clear() {
    ids_.clear();
    chunks_.clear();
    size_ = 0;
  }

  /// Reserves capacity for \p n chunks (not bits). Call before bulk
  /// insert/merge.
  void reserveChunks(std::size_t n) {
    ids_.reserve(n);
    chunks_.reserve(n);
  }

  [[nodiscard]] std::size_t getSize() const { return size_; }

  [[nodiscard]] bool isEmpty() const noexcept { return chunks_.empty(); }

  bool operator==(const ChunkedSparseBitsetPtsSet &other) const {
    return ids_ == other.ids_ && chunks_ == other.chunks_;
  }

  iterator begin() const { return iterator(&ids_, &chunks_, false); }
  iterator end() const { return iterator(&ids_, &chunks_, true); }

private:
  static constexpr std::uint64_t chunkId(Index idx) noexcept {
    return static_cast<std::uint64_t>(idx / kChunkBits);
  }

  static constexpr std::uint64_t chunkOffset(Index idx) noexcept {
    return static_cast<std::uint64_t>(idx % kChunkBits);
  }

  std::size_t findChunk(std::uint64_t chunk_id) const {
    const auto *it = std::lower_bound(ids_.begin(), ids_.end(), chunk_id);
    return static_cast<std::size_t>(it - ids_.begin());
  }

  static bool chunkAllZero(const Chunk &c) {
    for (std::uint64_t w : c.words)
      if (w)
        return false;
    return true;
  }

  static inline unsigned popcount(std::uint64_t value) {
#if defined(__clang__) || defined(__GNUC__)
    return static_cast<unsigned>(__builtin_popcountll(value));
#else
    unsigned count = 0;
    while (value) {
      value &= (value - 1);
      ++count;
    }
    return count;
#endif
  }

  static std::size_t chunkPopcount(const Chunk &c) {
    std::size_t n = 0;
    for (std::uint64_t w : c.words)
      n += static_cast<std::size_t>(popcount(w));
    return n;
  }

  IdsVec ids_;
  ChunksVec chunks_;
  std::size_t size_ = 0;
};

#endif // ANDERSEN_CHUNKED_SPARSE_BITSET_PTSSET_H
