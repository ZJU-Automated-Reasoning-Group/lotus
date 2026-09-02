// RUN: clang -O0 -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -check-memsafety -config=memregion %t2.bc > %t3
// RUN: grep -F "No possibly invalid memory accesses detected." %t3

#include <stdlib.h>

void test(void)
{
    int * p = malloc(4*sizeof(int));
    p[0] = 2;
    p[1] = 3;
    *p;
}
