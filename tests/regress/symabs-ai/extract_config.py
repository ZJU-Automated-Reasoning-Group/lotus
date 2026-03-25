#! /usr/bin/env python

import sys
import re

config_re = re.compile("(CONFIG:)(.*\n)")

def main(fin):
    for line in fin:
        assert line.endswith('\n')
        for _, config_line in config_re.findall(line):
            sys.stdout.write(config_line.strip() + '\n')


if __name__ == "__main__":
    with open(sys.argv[1], 'r') as fp:
        main(fp)
