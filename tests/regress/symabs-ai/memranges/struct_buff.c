// RUN: clang -g -O3 -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %symabs_ai -config=memrange -where=memory %t2.bc | grep 'load\|store' | sort > %t3
// RUN: false
// XFAIL: *

#include <stdint.h>

struct foo
{
	int size;
	char* buff;
};

void test(struct foo* ptr)
{
	ptr->buff[0] = 'A';
	ptr->buff[ptr->size-1] = 'Z';
}
