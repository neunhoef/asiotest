#!/bin/bash


gnuplot -p <<EOF
set style line 1 lc rgb '#0060ad' lt 1 lw 2 pt 7 ps 1.5   # --- blue
plot '$1' with linespoints ls 1
EOF