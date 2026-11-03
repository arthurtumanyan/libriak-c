#!/bin/sh
# Comparison of two buckets, on one cluster or (with PEER_URL) on two, as a
# check after a copy or a migration. Keys and clocks come from AAE folds in
# key ranges of at most CHUNK keys per bucket, so memory does not grow with
# the buckets. Prints one line per difference ("status key") and exits 1
# when the buckets differ.
#   operations/bucket-compare.sh BUCKET OTHER_BUCKET [BUCKET_TYPE]
set -eu
RIAKCTL=${RIAKCTL:-riakctl}
bucket=${1:?bucket}
other=${2:?other bucket}
type=${3:-}
CHUNK=${CHUNK:-20000}

diffs=$("$RIAKCTL" --compact ${type:+--type "$type"} aae compare "$bucket" \
    --peer-bucket "$other" ${PEER_URL:+--peer "$PEER_URL"} --chunk "$CHUNK" |
    jq -r '"\(.status) \(.key)"' | sort)
if [ -z "$diffs" ]; then
    echo "$bucket and $other are identical"
    exit 0
fi
echo "$diffs"
echo "$bucket and $other differ in $(echo "$diffs" | wc -l | tr -d ' ') key(s)"
exit 1
