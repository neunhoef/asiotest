#!/bin/bash

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <number_io> <from_worker> <to_worker>"
    echo "Spanws multiple servers at ports 81xx, where xx is the"
    echo "Number of workers."
    exit
fi

killall server-generic

for j in $(seq -f "%02g" $2 $3); do
	echo "./server-generic 83$i"
	./server-generic 83$j $1 $j 116000 4 &
done

#
