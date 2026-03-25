// RUN: clang -g -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %symabs_ai -config=memrange -where=memory %t2.bc | grep 'load\|store' | sort > %t3
// RUN: diff %t3 %s.reference

#include <stdint.h>

struct level3
{
	int32_t foo;
	char bar;
};

struct level2
{
	int foo;
	union {
		struct level3 down;
		char buff[64];
	};
};

struct level1
{
	int an_int;
	struct level2 down;
	int another_int;
};

int test(struct level1* ptr)
{
	ptr->down.down.bar = '?';
	return ptr->down.buff[4] == '?';
}

#ifdef CBMC
// can be verified with: cbmc -DCBMC <this_file>.c
int main()
{
	extern struct level1* source();
	assert(test(source()));
	return 0;
}
#endif
