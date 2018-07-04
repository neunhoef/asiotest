#!/bin/bash

if [ "$#" -ne 6 ]; then
    echo "Usage: $0 <port> <server_exec> <server_load> <tries_per_test> <client_threads> <msg_size>"
    exit 
fi

for i in {2..10..2}; do
	for j in {2..20..4}; do
		echo "Testing for $i IO and $j Worker" 
		$2 $1 $i $j $3 &
		pid=$!
		sleep 1s
		./client2 localhost $1 $4 $5 $6
		kill $pid
		wait $pid
	done
done
