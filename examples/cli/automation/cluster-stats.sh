#!/bin/sh
# Statistics sampler: selected /stats entries of the node as CSV, one row
# per sample, for spreadsheets or a time series import.
#   automation/cluster-stats.sh [INTERVAL_S [SAMPLES]] > stats.csv
#   STATS="node_gets node_puts" automation/cluster-stats.sh 10 6
set -eu
RIAKCTL=${RIAKCTL:-riakctl}
interval=${1:-5}
samples=${2:-1}
STATS=${STATS:-node_gets node_puts node_get_fsm_time_95 node_put_fsm_time_95 vnode_gets vnode_puts memory_total}

# shellcheck disable=SC2086
echo "time,$(echo $STATS | tr ' ' ',')"
i=0
while [ "$i" -lt "$samples" ]; do
    # shellcheck disable=SC2086
    "$RIAKCTL" --compact stats $STATS |
        jq -r --arg t "$(date -u +%Y-%m-%dT%H:%M:%SZ)" --arg names "$STATS" \
            '. as $s | [$t] + ($names | split(" ") | map($s[.] // "" | tostring)) | join(",")'
    i=$((i + 1))
    if [ "$i" -lt "$samples" ]; then
        sleep "$interval"
    fi
done
