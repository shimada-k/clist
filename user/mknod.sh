#!/bin/sh

#
#	rootで実行すること
#

MAJOR=$(awk "\$2==\"clist_benchmark\" {print \$1}" /proc/devices)

echo "$MAJOR"

rm -f /dev/clbench

mknod /dev/clbench c "$MAJOR" 0

