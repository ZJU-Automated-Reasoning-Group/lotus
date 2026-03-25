#include "Utils/ADT/WrappedInterval.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define DEBUG 0

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#define ASSERT_OR_ABORT(x, mex)                                                \
  if (unlikely(!(x))) {                                                        \
    fprintf(stderr, "[interval ABORT] " mex "\n");                             \
    abort();                                                                   \
  }
#define _min(a, b) ((a) < (b) ? (a) : (b))
#define _max(a, b) ((a) > (b) ? (a) : (b))

static inline int __attribute__((unused)) is_signed_op(optype op) {
  return !(op == OP_ULT || op == OP_ULE || op == OP_UGT || op == OP_UGE);
}

static inline uint64_t __attribute__((unused)) sext(uint64_t c, uint32_t size) {
  uint64_t mask = 1UL << (size - 1UL);
  if (c & mask) {
    c |= ((2UL << (64UL - size - 1UL)) - 1UL) << size;
    return c;
  }
  return c;
}

static inline uint64_t get_size_mask(uint32_t size) {
  uint64_t res = 0;
  res |= (2UL << (size - 1UL)) - 1UL;
  return res;
}

static inline uint64_t get_signed_min(uint32_t size) {
  if (size < 2)
    return 0;
  uint64_t res = 1UL << (size - 1UL);
  return res;
}

static inline uint64_t get_signed_max(uint32_t size) {
  if (size < 2)
    return 0;
  return get_signed_min(size) - 1;
}

static inline int is_wrapping(const wrapped_interval_t *interval) {
  return interval->min > interval->max;
}

static inline int is_right_of(uint64_t v1, uint64_t v2, uint32_t size) {
  return ((v1 - v2) & get_size_mask(size)) <= ((v2 - v1) & get_size_mask(size));
}

static inline int __attribute__((unused)) is_left_of(uint64_t v1, uint64_t v2,
                                                     uint32_t size) {
  return !is_right_of(v1, v2, size);
}

static inline int wi_check_intersect(const wrapped_interval_t *int1,
                                     const wrapped_interval_t *int2) {
  if (is_wrapping(int1) && is_wrapping(int2))
    return 1;
  if (!is_wrapping(int1) && is_wrapping(int2))
    return int1->min <= int2->max || int1->max >= int2->min;
  if (is_wrapping(int1) && !is_wrapping(int2))
    return int2->min <= int1->max || int2->max >= int1->min;
  return _max(int1->min, int2->min) <= _min(int2->max, int2->max);
}

int wi_intersect(wrapped_interval_t *int1, const wrapped_interval_t *int2) {
  // Overapproximation
  // -> side-effect on int1
#if DEBUG
  printf("intersection:\n 1) ");
  wi_print(int1);
  printf(" 2) ");
  wi_print(int2);
#endif
  if (!wi_check_intersect(int1, int2))
    return 0;

  if (!is_wrapping(int1) && !is_wrapping(int2)) {
    int1->min = _max(int1->min, int2->min);
    int1->max = _min(int1->max, int2->max);
  } else {
    if (wi_contains_element(int2, int1->max) &&
        !wi_contains_element(int2, int1->min)) {
      /*
       *        +--------+
       * +------|-+      |
       * |   I1 | | I2   |
       * +------|-+      |
       *        +--------+
       */
      int1->min = int2->min;
      // int1->max = int1->max;
    } else if (wi_contains_element(int2, int1->min) &&
               !wi_contains_element(int2, int1->max)) {
      /*
       *        +--------+
       * +------|-+      |
       * |   I2 | | I1   |
       * +------|-+      |
       *        +--------+
       */
      // int1->min = int1->min;
      int1->max = int2->max;
    } else {
      /*
       * ------+    +------
       *     +-|----|-+
       *  I1 | | I2 | | I1     (overapprox, take min (I1, I2))
       *     +-|----|-+
       * ------+    +------
       *
       * ------+    +------
       *     +-|----|-+
       *  I2 | | I1 | | I2     (overapprox, take min (I1, I2))
       *     +-|----|-+
       * ------+    +------
       *
       * +-----------------+
       * |   +--------+    |
       * |   |   I1   | I2 |   (no approximation)
       * |   +--------+    |
       * +-----------------+
       *
       * +-----------------+
       * |   +--------+    |
       * |   |   I2   | I1 |   (no approximation)
       * |   +--------+    |
       * +-----------------+

       */
      // double intersection -> possibly overapproximation
      if (wi_get_range(int2) < wi_get_range(int1)) {
        int1->min = int2->min;
        int1->max = int2->max;
      }
    }
  }
#if DEBUG
  printf(" -> ");
  wi_print(int1);
#endif
  return 1;
}

wrapped_interval_t wi_init(unsigned size) {
  ASSERT_OR_ABORT(size <= 64, "INTERVAL_H init_interval() - invalid size");

  wrapped_interval_t res = {.min = 0, .max = get_size_mask(size), .size = size};
  return res;
}

int wi_update_cmp(wrapped_interval_t *src, uint64_t c, optype op) {
  int res = 1;
  wrapped_interval_t cint = {.size = src->size};
  c &= get_size_mask(src->size);
  switch (op) {
  case OP_ULT:
    c--;
  case OP_ULE: {
    cint.min = 0;
    cint.max = c;
    res = wi_intersect(src, &cint);
    break;
  }
  case OP_SLT:
    c--;
  case OP_SLE: {
    cint.min = _max(get_signed_min(src->size), src->min);
    cint.max = c;
    res = wi_intersect(src, &cint);
    break;
  }
  case OP_UGT:
    c++;
  case OP_UGE: {
    cint.min = c;
    cint.max = _min(get_size_mask(src->size), src->max);
    res = wi_intersect(src, &cint);
    break;
  }
  case OP_SGT:
    c++;
  case OP_SGE: {
    cint.min = c;
    cint.max = _min(get_signed_max(src->size), src->max);
    res = wi_intersect(src, &cint);
    break;
  }
  case OP_EQ: {
    if (!wi_contains_element(src, c)) {
      res = 0;
      break;
    }
    src->min = c;
    src->max = c;
    break;
  }

  default:
    ASSERT_OR_ABORT(0, "INTERVAL_H interval_update_cmp() - invalid op");
    break;
  }
  return res;
}

void wi_update_add(wrapped_interval_t *src, uint64_t c) {
  src->min = (src->min + c) & get_size_mask(src->size);
  src->max = (src->max + c) & get_size_mask(src->size);
}

void wi_update_sub(wrapped_interval_t *src, uint64_t c) {
  src->min = (src->min - c) & get_size_mask(src->size);
  src->max = (src->max - c) & get_size_mask(src->size);
}

void wi_update_invert(wrapped_interval_t *src) {
  src->min = (-src->min) & get_size_mask(src->size);
  src->max = (-src->max) & get_size_mask(src->size);
  if (src->max < src->min) {
    src->min ^= src->max;
    src->max ^= src->min;
    src->min ^= src->max;
  }
}

void wi_modify_size(wrapped_interval_t *src, uint32_t new_size) {
  if (new_size < src->size) {
    wrapped_interval_t tmp = {
        .min = 0, .max = get_size_mask(new_size), .size = src->size};
    wi_intersect(src, &tmp);
  }
  src->size = new_size;
}

int wi_contains_element(const wrapped_interval_t *interval, uint64_t value) {
  if (is_wrapping(interval))
    return (interval->min <= value && value <= get_size_mask(interval->size)) ||
           (0 <= value && value <= interval->max);
  return interval->min <= value && value <= interval->max;
}

uint64_t wi_get_range(const wrapped_interval_t *interval) {
  uint64_t mask = get_size_mask(interval->size);
  uint64_t range = (interval->max & mask) - (interval->min & mask);
  return (uint64_t)(range & mask);
}

wrapped_interval_iter_t wi_init_iter_values(wrapped_interval_t *interval) {
  wrapped_interval_iter_t it;
  it.min_v = interval->min;
  it.curr_i = 0;
  it.max_i = wi_get_range(interval);
  it.mask = get_size_mask(interval->size);
  return it;
}

int wi_iter_get_next(wrapped_interval_iter_t *it, uint64_t *el) {
  if (it->curr_i > it->max_i)
    return 0;

  *el = (uint64_t)((it->min_v + it->curr_i++) & it->mask);
  return 1;
}

const char *op_to_string(optype op) {
  switch (op) {
  case OP_UGT:
    return "OP_UGT";
  case OP_UGE:
    return "OP_UGE";
  case OP_SGT:
    return "OP_SGT";
  case OP_SGE:
    return "OP_SGE";
  case OP_ULT:
    return "OP_ULT";
  case OP_ULE:
    return "OP_ULE";
  case OP_SLT:
    return "OP_SLT";
  case OP_SLE:
    return "OP_SLE";

  default:
    ASSERT_OR_ABORT(0, "op_to_string() - unexpected optype");
  }
}

void wi_print(wrapped_interval_t *interval) {
  if (!is_wrapping(interval))
    fprintf(stderr, "[ 0x%" PRIx64 ", 0x%" PRIx64 " ] (%u)\n", interval->min,
            interval->max, interval->size);
  else
    fprintf(stderr,
            "[ 0x%" PRIx64 ", 0x%" PRIx64 " ] U [ 0x%" PRIx64 ", 0x%" PRIx64
            " ] (%u)\n",
            interval->min, get_size_mask(interval->size), (uint64_t)0ULL,
            interval->max, interval->size);
}
