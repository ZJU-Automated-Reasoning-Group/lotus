#include "CFL/CSIndex/FLARE/BitVector.h"

#include <iostream>

namespace lotus::cfl::cs_index::flare {

bool findCommonOne(BitVector *a, BitVector *b) {
  int num_int64 = a->num_bytes / 8;
  int left_bytes = a->num_bytes % 8;
  uint64_t tmp = 0l;
  for (int i = 0; i < num_int64; i++) {
    tmp =
        static_cast<uint64_t *>(a->_db)[i] & static_cast<uint64_t *>(b->_db)[i];
    if (tmp != 0)
      return true;
  }
  for (int i = 0; i < left_bytes; i++) {
    tmp = static_cast<unsigned char *>(a->_db)[i] &
          static_cast<unsigned char *>(b->_db)[i];
    if (tmp != 0)
      return true;
  }
  return false;
}

BitVector::BitVector() : _num_bits(0), _db(NULL) { num_bytes = 0; }

BitVector::BitVector(int64_t num_bits) : _num_bits(num_bits), _db(NULL) {
  num_bytes = _num_bits / 8;
  if (_num_bits % 8 != 0) {
    num_bytes += 1;
  }

  if ((size_t)num_bytes < num_bytes) {
    std::cout << "Error: won't be able to calloc " << num_bytes
              << " bytes on architecture with sizeof(size_t) " << sizeof(size_t)
              << "\n";
  }

  if ((_db = calloc(num_bytes, 1)) == NULL) {
    std::cerr << "calloc BitVector()" << "\n";
    std::cerr << "num_bytes " << num_bytes << "\n";
    exit(1);
  }
}

BitVector::~BitVector() { free(_db); }

} // namespace lotus::cfl::cs_index::flare
