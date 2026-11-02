#!/bin/sh
# Backup of one bucket into a JSON Lines file, checked before it is kept:
# the trailer must be present and its count must match the entries.
#   basic/backup.sh BUCKET [FILE] [BUCKET_TYPE]
set -eu
RIAKCTL=${RIAKCTL:-riakctl}
bucket=${1:?bucket}
file=${2:-$bucket-$(date -u +%Y%m%dT%H%M%SZ).jsonl}
type=${3:-}

tmp=$file.partial
trap 'rm -f "$tmp"' EXIT
"$RIAKCTL" ${type:+--type "$type"} backup -o "$tmp" "$bucket"

count=$(tail -n 1 "$tmp" | jq -er 'select(.end) | .count') || {
    echo "$tmp: no trailer, the backup is incomplete" >&2
    exit 1
}
entries=$(jq -c 'select(.key)' "$tmp" | wc -l)
[ "$entries" -eq "$count" ] || { echo "trailer says $count entries, file has $entries" >&2; exit 1; }
siblings=$(jq -s '[.[] | select(.siblings) | .siblings | length | select(. > 1)] | length' "$tmp")
mv "$tmp" "$file"
trap - EXIT
echo "$file: $count entries, $siblings with siblings"
