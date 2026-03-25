// RUN: %extract_config %s > %t1.py
// RUN: clang -c -emit-llvm %s -o %t1.bc
// RUN: opt -instcombine -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %symabs_ai -check-assertions -config=%t1.py %t2.bc > %t3

// CONFIG: config.FragmentDecomposition = {'Strategy': 'Body'}
// CONFIG: config.AbstractDomain = [SimpleConstProp, NumRels.Unsigned(ParamStrategy.AllValuePairs())]

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
