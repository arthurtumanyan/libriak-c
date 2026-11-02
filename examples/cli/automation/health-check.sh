#!/bin/sh
# Monitoring check in the Nagios/Icinga convention: one status line, exit
# code 0 (OK), 1 (WARNING), 2 (CRITICAL) or 3 (UNKNOWN). Nodes that do not
# answer and slow 95th percentile GET/PUT times are reported.
#   automation/health-check.sh [WARN_US [CRIT_US]]   (defaults 50000 200000)
set -u
RIAKCTL=${RIAKCTL:-riakctl}
warn=${1:-50000}
crit=${2:-200000}

ping=$("$RIAKCTL" --compact ping 2>/dev/null)
rc=$?
if [ -z "$ping" ]; then
    echo "UNKNOWN - riakctl failed (exit code $rc)"
    exit 3
fi
up=$(echo "$ping" | jq .up)
total=$(echo "$ping" | jq .total)
if [ "$up" -eq 0 ]; then
    echo "CRITICAL - no node answers ($total configured)"
    exit 2
fi

stats=$("$RIAKCTL" --compact stats node_get_fsm_time_95 node_put_fsm_time_95 \
        vnode_gets vnode_puts ring_members 2>/dev/null) || {
    echo "UNKNOWN - statistics not available (HTTP port?)"
    exit 3
}
get95=$(echo "$stats" | jq '.node_get_fsm_time_95 // 0 | floor')
put95=$(echo "$stats" | jq '.node_put_fsm_time_95 // 0 | floor')
perf="get95=${get95}us;$warn;$crit put95=${put95}us;$warn;$crit nodes_up=$up;;;0;$total"

worst=$get95
[ "$put95" -gt "$worst" ] && worst=$put95
if [ "$up" -lt "$total" ]; then
    echo "WARNING - $up of $total nodes answer | $perf"
    exit 1
elif [ "$worst" -ge "$crit" ]; then
    echo "CRITICAL - 95th percentile latency ${worst}us | $perf"
    exit 2
elif [ "$worst" -ge "$warn" ]; then
    echo "WARNING - 95th percentile latency ${worst}us | $perf"
    exit 1
fi
echo "OK - $up/$total nodes, get95 ${get95}us, put95 ${put95}us | $perf"
