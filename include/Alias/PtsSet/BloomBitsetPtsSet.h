// Bitset + Bloom-filter points-to set.
//
// Representation:
// - Dynamic dense bitset for exact membership.
// - Fixed-size Bloom filter to accelerate negative checks for
// contains/intersect.
//
// This file is standalone and not wired into any analysis yet.
#ifndef ANDERSEN_BLOOM_BITSET_PTSSET_H
#define ANDERSEN_BLOOM_BITSET_PTSSET_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

template <std::size_t BloomBits = 4096, std::size_t Hashes = 3>
class BloomBitsetPtsSet {
public:
  using Index = std::uint64_t;

  static constexpr std::size_t kWordBits = 64;
  static constexpr std::size_t kBloomBits = BloomBits;
  static constexpr std::size_t kBloomWords = kBloomBits / kWordBits;
  static constexpr std::size_t kHashes = Hashes;

  static_assert(kBloomBits % kWordBits == 0,
                "Bloom filter size must be a multiple of word size.");
  static_assert(kHashes >= 1, "Bloom filter needs at least one hash.");

  class iterator {
  public:
    using value_type = Index;
    using difference_type = std::ptrdiff_t;
    using pointer = const Index *;
    using reference = const Index &;
    using iterator_category = std::forward_iterator_tag;

    iterator() = default;

    Index operator*() const { return current_; }

    iterator &operator++() {
      advance();
      return *this;
    }

    bool operator==(const iterator &other) const {
      return words_ == other.words_ && word_pos_ == other.word_pos_ &&
             bit_pos_ == other.bit_pos_ && end_ == other.end_;
    }

    bool operator!=(const iterator &other) const { return !(*this == other); }

  private:
    friend class BloomBitsetPtsSet;

    explicit iterator(const std::vector<std::uint64_t> *words, bool end)
        : words_(words), end_(end) {
      if (words_ && !end_) {
        word_pos_ = 0;
        bit_pos_ = 0;
        findNext();
      }
    }

    void advance() {
      if (end_)
        return;
      ++bit_pos_;
      findNext();
    }

    void findNext() {
      if (!words_ || words_->empty()) {
        end_ = true;
        return;
      }

      while (word_pos_ < words_->size()) {
        const std::uint64_t word = (*words_)[word_pos_];
        const std::uint64_t word_masked =
            (word & (~std::uint64_t{0} << bit_pos_));
        if (word_masked == 0) {
          ++word_pos_;
          bit_pos_ = 0;
          continue;
        }
        const int tz = countTrailingZeros(word_masked);
        const std::size_t next_bit = bit_pos_ + static_cast<std::size_t>(tz);
        current_ = static_cast<Index>(word_pos_ * kWordBits + next_bit);
        bit_pos_ = next_bit + 1;
        if (bit_pos_ >= kWordBits) {
          bit_pos_ = 0;
          ++word_pos_;
        }
        return;
      }

      end_ = true;
    }

    static int countTrailingZeros(std::uint64_t value) {
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

    const std::vector<std::uint64_t> *words_ = nullptr;
    std::size_t word_pos_ = 0;
    std::size_t bit_pos_ = 0;
    Index current_ = 0;
    bool end_ = true;
  };

  bool has(Index idx) const {
    if (!bloomMaybe(idx))
      return false;
    const std::size_t word = static_cast<std::size_t>(idx / kWordBits);
    const std::size_t bit = static_cast<std::size_t>(idx % kWordBits);
    if (word >= words_.size())
      return false;
    return (words_[word] >> bit) & 1U;
  }

  bool insert(Index idx) {
    const std::size_t word = static_cast<std::size_t>(idx / kWordBits);
    const std::size_t bit = static_cast<std::size_t>(idx % kWordBits);
    if (word >= words_.size())
      words_.resize(word + 1, 0);
    const std::uint64_t mask = std::uint64_t{1} << bit;
    if (words_[word] & mask) {
      updateBloom(idx);
      return false;
    }
    words_[word] |= mask;
    updateBloom(idx);
    return true;
  }

  bool contains(const BloomBitsetPtsSet &other) const {
    if (!bloomSuperset(other))
      return false;
    if (other.words_.size() > words_.size()) {
      for (std::size_t i = words_.size(); i < other.words_.size(); ++i) {
        if (other.words_[i] != 0)
          return false;
      }
    }
    const std::size_t limit = words_.size() < other.words_.size()
                                  ? words_.size()
                                  : other.words_.size();
    for (std::size_t i = 0; i < limit; ++i) {
      if ((words_[i] & other.words_[i]) != other.words_[i])
        return false;
    }
    return true;
  }

  bool intersectWith(const BloomBitsetPtsSet &other) const {
    if (!bloomIntersects(other))
      return false;
    const std::size_t limit = words_.size() < other.words_.size()
                                  ? words_.size()
                                  : other.words_.size();
    for (std::size_t i = 0; i < limit; ++i) {
      if ((words_[i] & other.words_[i]) != 0)
        return true;
    }
    return false;
  }

  bool unionWith(const BloomBitsetPtsSet &other) {
    if (other.words_.empty())
      return false;
    if (other.words_.size() > words_.size())
      words_.resize(other.words_.size(), 0);

    bool changed = false;
    for (std::size_t i = 0; i < other.words_.size(); ++i) {
      const std::uint64_t merged = words_[i] | other.words_[i];
      changed |= (merged != words_[i]);
      words_[i] = merged;
    }
    for (std::size_t i = 0; i < kBloomWords; ++i)
      bloom_[i] |= other.bloom_[i];
    return changed;
  }

  void clear() {
    words_.clear();
    bloom_.fill(0);
  }

  std::size_t getSize() const {
    std::size_t total = 0;
    for (std::uint64_t word : words_)
      total += static_cast<std::size_t>(popcount(word));
    return total;
  }

  bool isEmpty() const {
    for (std::uint64_t word : words_)
      if (word != 0)
        return false;
    return true;
  }

  bool operator==(const BloomBitsetPtsSet &other) const {
    return words_ == other.words_;
  }

  iterator begin() const { return iterator(&words_, false); }
  iterator end() const { return iterator(&words_, true); }

private:
  static std::uint64_t mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

  static std::uint64_t hash64(Index idx, std::uint64_t seed) {
    return mix(static_cast<std::uint64_t>(idx) ^ seed);
  }

  void updateBloom(Index idx) {
    const std::uint64_t h1 = hash64(idx, 0x1234abcdU);
    const std::uint64_t h2 = hash64(idx, 0x9e3779b9U) | 1U;
    for (std::size_t i = 0; i < kHashes; ++i) {
      const std::uint64_t h = h1 + i * h2;
      const std::size_t bit =
          static_cast<std::size_t>(h % static_cast<std::uint64_t>(kBloomBits));
      bloom_[bit / kWordBits] |= (std::uint64_t{1} << (bit % kWordBits));
    }
  }

  bool bloomMaybe(Index idx) const {
    const std::uint64_t h1 = hash64(idx, 0x1234abcdU);
    const std::uint64_t h2 = hash64(idx, 0x9e3779b9U) | 1U;
    for (std::size_t i = 0; i < kHashes; ++i) {
      const std::uint64_t h = h1 + i * h2;
      const std::size_t bit =
          static_cast<std::size_t>(h % static_cast<std::uint64_t>(kBloomBits));
      const std::uint64_t mask = std::uint64_t{1} << (bit % kWordBits);
      if ((bloom_[bit / kWordBits] & mask) == 0)
        return false;
    }
    return true;
  }

  bool bloomSuperset(const BloomBitsetPtsSet &other) const {
    for (std::size_t i = 0; i < kBloomWords; ++i) {
      if ((other.bloom_[i] & ~bloom_[i]) != 0)
        return false;
    }
    return true;
  }

  bool bloomIntersects(const BloomBitsetPtsSet &other) const {
    for (std::size_t i = 0; i < kBloomWords; ++i) {
      if ((bloom_[i] & other.bloom_[i]) != 0)
        return true;
    }
    return false;
  }

  static unsigned popcount(std::uint64_t value) {
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

  std::vector<std::uint64_t> words_;
  std::array<std::uint64_t, kBloomWords> bloom_{};
};

#endif // ANDERSEN_BLOOM_BITSET_PTSSET_H
