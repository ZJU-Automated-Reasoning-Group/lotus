// RUN: clang -c -emit-llvm %s -o %t1.bc
// RUN: opt -mem2reg -instnamer %t1.bc -o %t2.bc
// RUN: %lotus-verify-symabs-ai -check-assertions -config=... %t2.bc

#include <assert.h>

int test(int arg) {
    assert(arg == arg);
    return arg;
}
