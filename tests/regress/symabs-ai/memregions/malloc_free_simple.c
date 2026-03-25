// RUN: clang -O0 -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: not %symabs_ai -check-memsafety -config=memregion %t2.bc > %t3
// RUN: grep -F "1 possibly invalid memory access detected." %t3
// XFAIL: *
// Does not work because the memory state is only considered after the free()
// call

#include <stdlib.h>

void test(void)
{
    int * p = malloc(4*sizeof(int));
    p[0] = 2;
    p[1] = 3;
    *p;
    free(p);
    p[0] = 7;
}
