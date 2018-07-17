#!/bin/bash


gnuplot -p <<EOF
plot "submit_times.txt" using 0 title 'SubmitTimes' with lines
EOF
gnuplot -p <<EOF
plot "$1" using 3 title 'SuspendQueue' with lines
EOF
