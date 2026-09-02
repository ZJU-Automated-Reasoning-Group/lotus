// RUN: clang -c -emit-llvm %s -o %t1.bc
// RUN: opt -always-inline -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -check-assertions -config=01_const_function %t2.bc > %t3

#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

static uintptr_t __attribute__((always_inline)) align8(uintptr_t ptr)
{
	if (ptr % 8 == 0)
		return ptr;
	else
		return ptr + 8 - (ptr % 8);
}

// can be verified with: cbmc --function test <this_file>.c
void test(uintptr_t ptr, size_t size)
{
	if (size < 8 || ptr > UINTPTR_MAX - size)
		return;

	uintptr_t aligned_ptr = align8(ptr);

	assert(aligned_ptr >= ptr
		&& aligned_ptr < ptr + size
		&& aligned_ptr % 8 == 0);
}
