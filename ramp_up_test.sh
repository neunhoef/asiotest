#!/bin/bash


for i in {1..801..10}; do
    echo ./client2 $1 $2 10000 $i 60
    ./client2 $1 $2 10000 $i 60 | grep Reqs/s
done
