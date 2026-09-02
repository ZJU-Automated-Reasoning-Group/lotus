// RUN: clang -c -emit-llvm %s -o %t1.bc
// RUN: opt -always-inline -O3 -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -check-assertions -config=13_const_function_little_endian %t2.bc > %t3

#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

struct buff
{
	uintptr_t buff_start;
	int buff_size;
	uintptr_t data; // pointer inside [buff_start, buff_start+buff_end-1]
	int data_len;
};

static int __attribute__((always_inline)) buff_invariant(struct buff* buff)
{
	return buff->buff_size > 0
		&& buff->buff_start != 0
		&& buff->data >= buff->buff_start
		&& buff->data <= buff->buff_start + buff->buff_size
		&& buff->data_len >= 0
		&& buff->data + buff->data_len <= buff->buff_start + buff->buff_size;
}

static void __attribute__((always_inline)) do_stuff(struct buff* b)
{
	if (b->data_len < 8)
		return;

	b->data += 8;
	int remaining = b->buff_start + b->buff_size - b->data;

	if (remaining >= 8)
		b->data_len = 8;
	else
		b->data_len = remaining;
}

void test(struct buff* buff)
{
	// assume invariant holds for buff
	if (!buff_invariant(buff))
		return;

	// modify the structure
	do_stuff(buff);

	// prove that the invariant is preserved
	assert(buff_invariant(buff));
}

#ifdef CBMC
// can be verified with: cbmc -DCBMC <this_file>.c
int main()
{
	extern unsigned nondet_unsigned();

	struct buff b;
	b.buff_size = nondet_unsigned();
	b.buff_start = (uintptr_t) malloc(b.buff_size);
	b.data = b.buff_start + nondet_unsigned();
	b.data_len = nondet_unsigned();

	_CPROVER_ASSUME(buff_invariant(&b));
	test(&b);

	free((void*) b.buff_start);
	return 0;
}
#endif
