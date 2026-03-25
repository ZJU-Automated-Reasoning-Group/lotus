#ifndef WRAPPED_INTERVAL_H
#define WRAPPED_INTERVAL_H

#include <stdint.h>

#define OP_ULT 0
#define OP_SLT 1
#define OP_ULE 2
#define OP_SLE 3
#define OP_UGT 4
#define OP_SGT 5
#define OP_UGE 6
#define OP_SGE 7
#define OP_EQ 8

typedef int optype;

typedef struct wrapped_interval_t {
  uint64_t min;
  uint64_t max;
  uint32_t size;
} wrapped_interval_t;

typedef struct wrapped_interval_iter_t {
  uint64_t curr_i;
  uint64_t max_i;
  uint64_t min_v;
  uint64_t mask;
} wrapped_interval_iter_t;

wrapped_interval_t wi_init(unsigned size);
int wi_update_cmp(wrapped_interval_t *src, uint64_t c, optype op);
void wi_update_add(wrapped_interval_t *src, uint64_t c);
void wi_update_sub(wrapped_interval_t *src, uint64_t c);
void wi_update_invert(wrapped_interval_t *src);
void wi_modify_size(wrapped_interval_t *src, uint32_t new_size);
int wi_intersect(wrapped_interval_t *int1, const wrapped_interval_t *int2);
int wi_contains_element(const wrapped_interval_t *interval, uint64_t value);
uint64_t wi_get_range(const wrapped_interval_t *interval);
void wi_print(wrapped_interval_t *interval);

wrapped_interval_iter_t wi_init_iter_values(wrapped_interval_t *interval);
int wi_iter_get_next(wrapped_interval_iter_t *it, uint64_t *el);

const char *op_to_string(optype op);

#endif