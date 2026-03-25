// RUN: clang -g -O3 -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %symabs_ai -config=memrange -where=memory %t2.bc | grep 'load\|store' | sort > %t3
// RUN: diff %t3 %s.reference

#include <stdint.h>

struct foo
{
	int an_int;
	float a_float;
	int another_int;
};

void test(struct foo* ptr)
{
	if (ptr->an_int == 0)
		ptr->an_int = ptr->another_int;
}
