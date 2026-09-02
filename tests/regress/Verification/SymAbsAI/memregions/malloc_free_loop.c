// RUN: clang -O0 -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: not %lotus-verify-symabs-ai -check-memsafety -config=memregion %t2.bc > %t3
// RUN: grep -F "1 possibly invalid memory access detected." %t3

#include <stdlib.h>

void test(void)
{
    int * p = malloc(6*sizeof(int));
    for (int i = 0; i < 6; i++) {
        p[i] = i;
    }
    free(p);
    p[0] = 7;
}
