#!/bin/bash
N=${N:-300}
# wait for the compositor to give us the final (tiled) size
for i in $(seq 1 30); do c=$(tput cols); [ "$c" != 80 ] && break; sleep 0.05; done
sleep 0.3
cols=$(tput cols); rows=$(tput lines)
line=""
for ((i=0;i<cols;i++)); do
  case $((i%7)) in
    0) line+=$'\e[31m';; 1) line+=$'\e[1m';; 2) line+=$'\e[0m';; 3) line+="-";; 4) line+=">";; 5) line+="=";; 6) line+="a";;
  esac
  line+="${i:0:1}"
done
line=${line:0:$cols}
screen=""
for ((r=0;r<rows-1;r++)); do screen+="$line"$'\e[0m\n'; done
start=$(date +%s.%N)
for ((k=0;k<N;k++)); do
  printf '\e[H%s' "$screen"
  sleep 0.004
done
end=$(date +%s.%N)
echo "fullscreen x$N: $(echo "$end - $start" | bc) s  (${cols}x${rows})" >> /tmp/bench.out
# scrolling workload
start=$(date +%s.%N)
seq 1 300000
end=$(date +%s.%N)
echo "seq300k: $(echo "$end - $start" | bc) s" >> /tmp/bench.out
sleep 0.2
