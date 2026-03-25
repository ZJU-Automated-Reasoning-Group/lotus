// RUN: clang -O0 -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: not %symabs_ai -check-memsafety -config=memregion %t2.bc > %t3
// RUN: grep -F "1 possibly invalid memory access detected." %t3

#include <stdlib.h>

void test(void)
{
    int * p = malloc(4*sizeof(int));
    p[0] = 2;
    p[1] = 3;
    p[3] = 42;
    p[4] = 17;
    p[3];
}
