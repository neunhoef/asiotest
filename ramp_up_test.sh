#!/bin/bash


for i in {1..20}; do
    ./client4 $1 $2  50000 $i 1 60 20 1 | grep Reqs/s
done
