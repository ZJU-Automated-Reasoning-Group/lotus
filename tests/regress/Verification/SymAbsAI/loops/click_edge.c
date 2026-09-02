// RUN: clang -c -emit-llvm %s -o %t1.bc
// RUN: opt -instcombine -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -check-assertions -config=05_const_rel_edge %t2.bc > %t3

#include <assert.h>

extern int dontknow(void);
extern int read(void);

void test(void)
{
	int x = 1;
	int z = read();
	int y = z;
	while (dontknow()) {
		if (y != z)
			x = 2;
		x = 2 - x;
		if (x != 1)
			y = 2;
	}
	assert(x == 1);
}
