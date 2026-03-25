/**
 * RUN: clang -O3 -c -emit-llvm %s -o %t1.clang.bc
 * RUN: opt -mem2reg -instnamer %t1.clang.bc -o %t1.bc
 * RUN: %extract_config %s > %t1.py
 * RUN: %symabs_ai -where=everywhere -config=%t1.py -function=test %t1.bc | %probe_filter %t1.bc > %t2
 * RUN: grep 'probe1 >= probe2\|probe2 =< probe1' %t2
 * XFAIL: *
 *
 * CONFIG: config.FragmentDecomposition = {'Strategy': 'Body'}
 * CONFIG: config.FunctionContext = {'AssumeNoUndef': True}
 * CONFIG: config.AbstractDomain = [NumRels, NumRels(ParamStrategy.ConstOffsets()]
 */

extern void sprattus_probe(int, ...);

void test(int* a, int N)
{
	int sum = 0;
	int k = 0;

	for (int i = 0; i < N; i = i + 2) {
		sprattus_probe(0, i, k, k <= i);

		if (a[i] < 0) {
			sum += a[k];
			k++;
		}
	}
}
