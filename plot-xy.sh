#!/bin/bash


gnuplot -p <<EOF
plot "$1" using 2 title 'WorkQueue' with lines
EOF
gnuplot -p <<EOF
plot "$1" using 3 title 'SuspendQueue' with lines
EOF