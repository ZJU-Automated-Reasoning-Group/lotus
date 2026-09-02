// RUN: %extract_config %s > %t1.py
// RUN: clang -c -O3 -emit-llvm %s -o %t1-pre.bc
// RUN: opt -mem2reg -instnamer %t1-pre.bc > %t1.bc
// RUN: %lotus-verify-symabs-ai -check-assertions -config=%t1.py %t1.bc > %t2

// CONFIG: config.FragmentDecomposition = {'Strategy': 'Function'}
// CONFIG: config.FloatingPointModel = 'IEEE'
// CONFIG: config.AbstractDomain = [globals()['SimpleConstProp']]

#include <assert.h>

void test(int a, int percent)
{
    if (a >= 10 && a < 20 && percent >= 0 && percent <= 100) {
        double fp_a = a, fp_ratio = percent / 100.0;
        double fp_result = fp_a * fp_ratio;
        int int_result = (a * percent) / 100;
        assert(int_result <= (int)fp_result && (int)fp_result <= int_result+1);
    }
}
