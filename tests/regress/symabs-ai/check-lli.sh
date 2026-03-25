#! /bin/bash
#
# check-lli.sh
#
lli $1
[ $? -eq $2 ]
