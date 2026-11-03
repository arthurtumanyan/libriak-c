#!/bin/sh
# nextgenrepl queue workflow: the keys and clocks of a bucket are read from
# AAE and pushed onto a replication queue (a full sync of one bucket, as a
# sink cluster would consume it), then the queue is drained and the changes
# counted. Needs a source queue (replrtq_srcqueue) on the node.
#   operations/replication-queues.sh BUCKET QUEUE [BUCKET_TYPE]
set -eu
RIAKCTL=${RIAKCTL:-riakctl}
bucket=${1:?bucket}
queue=${2:?queue name}
type=${3:-}
r() { "$RIAKCTL" --compact ${type:+--type "$type"} "$@"; }

# changes already waiting are taken off first
r repl fetch "$queue" >/dev/null 2>&1
pushed=$(r aae keys "$bucket" | r repl push "$queue")
echo "pushed:  $(echo "$pushed" | jq -r '"\(.pushed) keys, queue lengths fold \(.fold_length // 0) fsync \(.fsync_length // 0) realtime \(.realtime_length // 0)"')"
taken=$(r repl fetch "$queue" 2>/dev/null | jq -s '{changes: length, deleted: map(select(.deleted)) | length}')
echo "fetched: $(echo "$taken" | jq -r '"\(.changes) changes (\(.deleted) deletions)"')"
[ "$(echo "$taken" | jq .changes)" -ge "$(echo "$pushed" | jq .pushed)" ]
