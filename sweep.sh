#!/bin/bash
echo "drop_prob,timeout_ms,sent,received,bytes,dropped,retransmitted,timeouts,maxretx,wallsec,throughput" > results.csv
for d in 0.01 0.02 0.04 0.08; do
    for t in 150 175 200 225; do
        make clean && make DROP_PROB=$d TIMEOUT_MS=$t TARGET=2000 -s
        result=$(./demo 2>/dev/null | grep "STATS:" | cut -d' ' -f2)
        echo "$d,$t,$result" >> results.csv
        echo "Done: DROP_PROB=$d TIMEOUT_MS=$t → $result"
    done
done