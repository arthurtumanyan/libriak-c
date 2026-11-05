#!/bin/sh
# nextgenrepl queue workflow: the keys and clocks of a bucket are read from
# AAE and pushed onto a replication queue (a full sync of one bucket, as a
# sink cluster would consume it), then the queue is drained and the changes
# counted. Needs a source queue (replrtq_srcqueue) on the node.
# A key deleted shortly before can still be listed by AAE and pushed, but
# once its tombstone is reaped the node skips it on the fetch, so every key
# still stored after the fetch has to arrive.
#   operations/replication-queues.sh BUCKET QUEUE [BUCKET_TYPE]
set -eu
RIAKCTL=${RIAKCTL:-riakctl}
bucket=${1:?bucket}
queue=${2:?queue name}
type=${3:-}
r() { "$RIAKCTL" --compact ${type:+--type "$type"} "$@"; }

# changes already waiting are taken off first
r repl fetch "$queue" >/dev/null 2>&1
keys=$(r aae keys "$bucket")
pushed=$(echo "$keys" | r repl push "$queue")
echo "pushed:  $(echo "$pushed" | jq -r '"\(.pushed) keys, queue lengths fold \(.fold_length // 0) fsync \(.fsync_length // 0) realtime \(.realtime_length // 0)"')"
taken=$(r repl fetch "$queue" 2>/dev/null | jq -s '{changes: length, deleted: map(select(.deleted)) | length}')
echo "fetched: $(echo "$taken" | jq -r '"\(.changes) changes (\(.deleted) deletions)"')"
# exit code 4: siblings, stored as well
stored=$(echo "$keys" | jq -r .key | while read -r k; do
    rc=0
    r get "$bucket" "$k" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 0 ] || [ "$rc" -eq 4 ]; then echo "$k"; fi
done | wc -l)
[ "$(echo "$taken" | jq .changes)" -ge "$stored" ]
