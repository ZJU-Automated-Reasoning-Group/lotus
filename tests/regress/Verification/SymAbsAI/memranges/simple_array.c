// RUN: clang -g -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -config=memrange -where=memory %t2.bc | grep 'load\|store' | sort > %t3
// RUN: diff %t3 %s.reference

#include <stdint.h>

void test(uint8_t buff[])
{
	buff[7] = 17;
	if (buff[13] == buff[3])
		buff[0] = 0;
}
