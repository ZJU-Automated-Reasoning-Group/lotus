/**
 * RUN: clang -c -emit-llvm %s -o %t1.clang.bc
 * RUN: opt -mem2reg -instnamer %t1.clang.bc -o %t1.bc
 * RUN: %extract_config %s > %t1.py
 * RUN: %lotus-verify-symabs-ai -config=%t1.py -function=foo %t1.bc | %probe_filter %t1.bc > %t2
 * RUN: grep 'probe1 >S n\|n <S probe1' %t2
 *
 * CONFIG: config.ModuleContext = {'Recursive': True}
 * CONFIG: config.FragmentDecomposition = {'Strategy': 'Body'}
 * CONFIG: config.FunctionContext = {'AssumeNoUndef': True}
 * CONFIG: config.AbstractDomain = [NumRels(ParamStrategy.AllValuePairs(True)), domains['BitMask/Single']]
 */

extern void sprattus_probe(int, ...);

int sum(int n) {
    int acc = 0;

    for (int i = 0; acc <= n; i++) {
        acc += i;
    }

    return acc;
}

void foo(int n)
{
    int res = sum(n | 1) + sum(n);
    sprattus_probe(0, res);
}
