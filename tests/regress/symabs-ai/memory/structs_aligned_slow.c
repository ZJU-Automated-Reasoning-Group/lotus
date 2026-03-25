// RUN: clang -c -emit-llvm %s -o %t1.bc
// RUN: opt -always-inline -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %symabs_ai -check-assertions -config=10_const_function_aligned -function=test %t2.bc > %t3

#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

struct foo
{
	uint64_t size;
	uint64_t array[32];
};

void test()
{
	struct foo x;
	x.size = 5;
	x.array[4] = 1;
	x.array[1] = 4;
	assert(x.array[x.array[1]] == 1);
}
