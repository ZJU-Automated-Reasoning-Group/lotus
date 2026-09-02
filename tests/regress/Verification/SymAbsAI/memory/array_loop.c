// RUN: %extract_config %s > %t1.py
// RUN: clang -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -check-assertions -config=%t1.py -function=test %t2.bc > %t3

// CONFIG: config.MemoryModel = {'AddressBits': 8, 'Variant': 'LittleEndian'}
// CONFIG: config.FragmentDecomposition = {'Strategy': 'Headers'}
// CONFIG: config.AbstractDomain = [globals()['SimpleConstProp']]

#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

void test(int64_t x[16])
{
	int64_t five = 5;

	for (int i = 0; i < 16; i++) {
		if (x[i] != 5)
			continue;

		five = x[i];
	}

	assert(five == 5);
}
