#!/usr/bin/python

import sys
from termcolor import colored, cprint

warning = colored('Warning:', 'red', attrs=['bold'])

with open(sys.argv[1]) as result:
    #Read everything into one list
    lines = result.readlines()
    #Remove newline characters
    lines = [elem.strip('\n') for elem in lines]

    for line  in lines:
       keyvalue = line.split(" -> ")
       bval = colored (keyvalue[0], attrs=['bold'])

       if keyvalue[1] == "bottom":
           print(warning)
           print(bval + " may be mapped to bottom")
           continue
       if len(sys.argv) > 2:
           if keyvalue[0] == sys.argv[2]:
               if keyvalue[1] == "top":
                   cprint (keyvalue[0] + " is mapped to top. This is sound but maybe unintended", 'yellow')
                   sys.exit(0)
               else:
                   if keyvalue[1] == sys.argv[3]:
                       cprint ("Success", 'green')
                       sys.exit(0)
                   else:
                       cprint ("Fail! Wrong value for "+keyvalue[0], 'red')
                       cprint ("Value was "+keyvalue[1] + " but should be "+ sys.argv[3])
                       sys.exit(0)
