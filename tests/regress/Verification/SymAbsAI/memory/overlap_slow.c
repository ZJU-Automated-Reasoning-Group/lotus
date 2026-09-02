// RUN: %extract_config %s > %t1.py
// RUN: clang -c -emit-llvm %s -o %t1.bc
// RUN: opt -always-inline -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -check-assertions -config=%t1.py %t2.bc > %t3
//
// CONFIG: config.MemoryModel = {'AddressBits': -1, 'Variant': 'LittleEndian'}
// CONFIG: config.FragmentDecomposition = {'Strategy': 'Function'}
// CONFIG: config.Analyzer = {'Incremental': False, 'Variant': 'UnilateralAnalyzer'}
// CONFIG: config.AbstractDomain = [globals()['SimpleConstProp']]

#include <stdint.h>
#include <assert.h>
#include <stdlib.h>


void test(uint8_t* ptr)
{
	uint32_t* as_32 = (uint32_t*) ptr;
	// 00 00 ef be ad de 00 00
	as_32[0] = 0xbeef0000;
	as_32[1] = 0x0000dead;
	uint32_t* db = (uint32_t*) (ptr + 2);
	assert(*db == 0xdeadbeef);
}

#ifdef CBMC
// can be verified with: cbmc -DCBMC --function cbmc_test <this_file>.c
void cbmc_test()
{
	uint8_t arr[128];
	test(arr);
}
#endif
