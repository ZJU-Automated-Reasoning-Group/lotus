#ifndef __BIT_VECTOR_HH
#define __BIT_VECTOR_HH

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace lotus::cfl::cs_index::flare {

using namespace std;

class BitVector {
  friend bool findCommonOne(BitVector *a, BitVector *b);

public:
  BitVector();
  BitVector(int64_t num_bits);
  ~BitVector();

  void set_one(int64_t);

  void set_zero(int64_t);

  void reset();

  bool get(int64_t) const;

  int64_t num_bits_set() const;

  int64_t num_ones() const;

  void print() const;

protected:
  int64_t _num_bits;
  uint64_t num_bytes;
  void *_db;
};

inline void BitVector::reset() { memset(_db, 0, num_bytes); }

inline void BitVector::print() const {
  cout << "_num_bits=" << _num_bits << '\n';
  for (int64_t i = _num_bits - 1; i >= 0; i--) {
    cout << get(i);
    if (i % 8 == 0)
      cout << " ";
  }
  cout << '\n';
}

inline int64_t BitVector::num_bits_set() const { return _num_bits; }

inline int64_t BitVector::num_ones() const {
  int64_t num = 0;
  for (int64_t i = 0; i < _num_bits; i++)
    if (get(i))
      num++;
  return num;
}

inline void BitVector::set_one(int64_t bit_idx) {
  static_cast<unsigned char *>(_db)[bit_idx >> 3] |= (1 << (bit_idx & 7));
}

inline void BitVector::set_zero(int64_t bit_idx) {
  static_cast<unsigned char *>(_db)[bit_idx >> 3] &= ~(1 << (bit_idx & 7));
}

inline bool BitVector::get(int64_t bit_idx) const {
  return (static_cast<unsigned char *>(_db)[bit_idx >> 3] >> (bit_idx & 7)) & 1;
}


} // namespace lotus::cfl::cs_index::flare

#endif
