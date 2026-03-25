// RUN: clang -g -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %symabs_ai -config=memrange -where=memory %t2.bc | grep 'load\|store' | sort > %t3
// RUN: diff %t3 %s.reference

#include <stdint.h>

extern uint32_t nondet();

void test(uint8_t buff[], int32_t n)
{
	for (int32_t i = 0; i < n; i++)
		buff[i]++;
}
