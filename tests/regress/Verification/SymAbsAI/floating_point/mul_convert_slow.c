// RUN: %extract_config %s > %t1.py
// RUN: clang -c -O0 -emit-llvm %s -o %t1-pre.bc
// RUN: opt -mem2reg -instnamer %t1-pre.bc > %t1.bc
// RUN: %lotus-verify-symabs-ai -check-assertions -config=%t1.py %t1.bc > %t2

// CONFIG: config.FragmentDecomposition = {'Strategy': 'Function'}
// CONFIG: config.FloatingPointModel = 'IEEE'
// CONFIG: config.AbstractDomain = [globals()['SimpleConstProp']]

#include <assert.h>

int test(double x)
{
	if (x != 0)
		x = 0;

	double foo = (3.14 * (x + 1.0)) * 100;
	int res = (int)(foo);
	assert(res == 314);
	return res;
}
