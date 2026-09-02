import struct
import string


def double_to_smt(x):
    """Converts a IEEE double to an smt-lib notation compatible with Z3"""

    as_ull = struct.unpack('!Q', struct.pack('!d', x))[0]
    bits = format(as_ull, '064b')
    exponent = bits[1:12]
    significand = bits[12:]

    # convert significand to a hex notation
    significand = format(int(significand, 2), '013x')

    return '(fp #b' + bits[0] + ' #b' + exponent + ' #x' + significand + ')'


def gen_literal_test():
    nums = ['1.0', '-1.0', '2.0', '-2.0', '+0.0', '-0.0', '+inf', '-inf',
            '4.9406564584124654e-324', '-4.9406564584124654e-324']

    for (var, num) in zip(string.ascii_letters, nums):
        print('; RUN: grep \'%s -> %s\' %%t2' % (var, double_to_smt(float(num))))

    for (var, num) in zip(string.ascii_letters, nums):
        print('%%%s = fadd double 0.0, %s' % (var, num))
