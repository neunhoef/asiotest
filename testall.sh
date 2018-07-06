#!/bin/bash

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <addr> <from_worker> <to_worker> <tries>"
    echo "Spanws multiple clients the test at port 83xx"
    exit
fi

echo "Testing $1 from $2 to $3 with $4 tries"

for j in $(seq -f "%02g" $2 $3); do
    echo "Testing on 83$i"
    /client2 $1 83$i 50000000 800 60 | grep Reqs
done
