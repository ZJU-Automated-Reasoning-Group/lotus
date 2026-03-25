// RUN: %extract_config %s > %t1.py
// RUN: clang -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %symabs_ai -check-assertions -config=%t1.py -function=test %t2.bc > %t3

// CONFIG: config.MemoryModel = {'Variant': 'LittleEndian'}
// CONFIG: config.FragmentDecomposition = {'Strategy': 'Function'}
// CONFIG: config.Analyzer = {'Incremental': False, 'Variant': 'UnilateralAnalyzer'}
// CONFIG: config.AbstractDomain = [globals()['SimpleConstProp']]

#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

void test()
{
	int64_t x = 5;
	uintptr_t addr = (uintptr_t) &x;
	addr = (addr - 15);
	addr = addr + 7;
	int64_t* ptr = (int64_t*) addr;
	ptr++;
	assert(*ptr == 5);
}
