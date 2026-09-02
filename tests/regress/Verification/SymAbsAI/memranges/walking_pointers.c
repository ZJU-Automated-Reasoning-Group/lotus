/**
 * RUN: clang -O0 -g -c -emit-llvm %s -o %t1.clang.bc
 * RUN: opt -instcombine -simplifycfg -mem2reg -instnamer %t1.clang.bc -o %t1.opt.bc
 * RUN: %sprinstrument -config=memrange %t1.opt.bc %t2.db %t2.bc
 * RUN: clang %t2.bc %rt_flags -o %t2
 *
 * RUN: %lotus-verify-symabs-ai -config=memrange -where=memory -function=test %t1.opt.bc > %t2.static
 * RUN: %t2
 * RUN: %lotus-verify-symabs-ai -where=memory -function=test %t2.bc > %t2.hybrid
 * RUN: grep -F '[24:9] load *[buff, buff + n]' %t2.static
 * RUN: grep -F '[24:9] load *[buff, buff + n]' %t2.hybrid
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

uint8_t* test(uint8_t buff[], uint64_t n)
{
	uint8_t *end = buff + n;
	uint8_t *ptr = buff;

	while (*ptr != 0)
	{
		if (ptr != end)
			ptr++;
		else
			return NULL;
	}

	return ptr;
}

int main(int argc, char** argv)
{
	uint8_t buff[64];
	memset(buff, 7, sizeof(buff));
	uint8_t *ptr = &buff[32];
	assert(*ptr == 7);
	*ptr = 0;

	assert(test(buff, 1) == NULL);
	assert(test(buff, 31) == NULL);
	assert(test(buff, 32) == ptr);
	assert(test(buff, UINT64_MAX) == ptr);

	return 0;
}
