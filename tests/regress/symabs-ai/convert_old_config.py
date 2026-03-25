#!/usr/bin/env python2

import sys
import yaml
import re

config_re = re.compile("^(.*CONFIG:)(.*)$")

def main(file_name):
    lines_pre = []
    lines_post = []
    yaml_src = ''
    prefix = None

    with open(file_name) as fp:
        for line in fp:
            m = config_re.match(line)
            if m is None:
                if prefix is None:
                    lines_pre.append(line)
                else:
                    lines_post.append(line)
            else:
                yaml_src += '\n' + m.group(2)
                prefix = m.group(1)


    yaml_dict = yaml.load(yaml_src)

    with open(file_name, 'w') as out:
        for x in lines_pre:
            out.write(x)

        domains = yaml_dict['AbstractDomain']
        del yaml_dict['AbstractDomain']

        for k, v in yaml_dict.items():
            out.write('%s config.%s = %s\n' % (prefix, k, v))

        domlist = ', '.join('globals()[\'%s\']' % x for x in domains)
        out.write('%s config.AbstractDomain = [%s]\n' % (prefix, domlist))

        for x in lines_post:
            out.write(x)


if __name__ == '__main__':
    main(sys.argv[1])
