// RUN: clang -c -g -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -check-memsafety -config=memregion %t2.bc > %t3
// RUN: grep -F "No possibly invalid memory accesses detected." %t3

#include <stdint.h>

void test(void)
{
	struct S {
		uint8_t a;
		uint8_t b;
	} s;

	s.a = 42;
	s.b = 17;
	s.a = s.a + s.b;
}
