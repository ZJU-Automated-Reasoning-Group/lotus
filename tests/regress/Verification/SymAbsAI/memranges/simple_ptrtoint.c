// RUN: clang -g -O3 -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -config=memrange -where=memory %t2.bc | grep 'load\|store' | sort > %t3
// RUN: diff %t3 %s.reference

#include <stdint.h>

void test(int* buff)
{
	uintptr_t ptr = (uintptr_t) buff;
	ptr += 7;
	*(int*) ptr = 17;
	ptr += 12;
	if (*(char*) ptr == '!') {
		*(char*) (--ptr) = '?';
	}
}
