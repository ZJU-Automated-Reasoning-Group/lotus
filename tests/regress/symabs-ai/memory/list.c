// RUN: %extract_config %s > %t1.py
// RUN: clang -O2 -c -emit-llvm %s -o %t1.bc
// RUN: opt -instcombine -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %symabs_ai -check-assertions -config=%t1.py -function=test %t2.bc > %t3

// CONFIG: config.MemoryModel = {'Variant': 'Aligned'}
// CONFIG: config.FragmentDecomposition = {'Strategy': 'Body'}
// CONFIG: config.AbstractDomain = [globals()['LegacyNull']]

// XFAIL: *
// TODO implement != Null domain
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

struct node {
	int64_t value;
	struct node* next;
};

void test(struct node* node, int64_t x)
{
	if (node == NULL)
		return;

	while (node->next != NULL) {
		if (node->value == x)
			break;

		node = node->next;
	}

	assert(node != NULL);
}
