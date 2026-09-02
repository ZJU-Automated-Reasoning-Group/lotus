/**
 * RUN: clang -c -emit-llvm %s -o %t1.clang.bc
 * RUN: opt -mem2reg -instnamer %t1.clang.bc -o %t1.bc
 * RUN: ...?
 */
#include <stdint.h>

extern void sprattus_probe(int, ...);

uint64_t foo(uint8_t a)
{
    uint64_t x = a;
    x = x | (x << 32);
    x = x | (x << 16) | (x << 48);

    sprattus_probe(0, x);
    return x;
}

int main(void)
{
    foo(0xAA);
    return 0;
}
