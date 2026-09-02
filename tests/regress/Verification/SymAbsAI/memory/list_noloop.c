// Partially-unrolled version of `list.c`
//
// RUN: clang -O3 -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -check-assertions -config=13_const_function_little_endian -function=test %t2.bc > %t3

#include <stdlib.h>
#include <assert.h>

struct node {
	int value;
	struct node* next;
};

void test(struct node* node, int x)
{
	if (node == NULL)
		return;

	if (node->next != NULL) {
		assert(node != NULL);

		if (node->value == x)
			goto end;

		node = node->next;
	}
	if (node->next != NULL) {
		assert(node != NULL);

		if (node->value == x)
			goto end;

		node = node->next;
	}
	if (node->next != NULL) {
		assert(node != NULL);

		if (node->value == x)
			goto end;

		node = node->next;
	}

end:
	assert(node != NULL);
}
