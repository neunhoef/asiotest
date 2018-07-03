#!/bin/bash

killall server3
for i in {2..10..2}; do
	for j in {2..20..4}; do
		#echo "Testing for $i IO and $j Worker" 
		./server3 7777 $i $j 1 &
		pid=$!
		sleep 1s
		./client2 localhost 7777 50000 512 1
		kill -s USR2 $pid
		wait $pid
	done
done
