#!/bin/sh
# Restore of a backup made by basic/backup.sh, optionally into another
# bucket. A dry run is shown first; restoring twice changes nothing.
#   basic/restore.sh FILE [BUCKET [BUCKET_TYPE]]
set -eu
RIAKCTL=${RIAKCTL:-riakctl}
file=${1:?backup file}
bucket=${2:-}
type=${3:-}

opts="${bucket:+--bucket $bucket} ${type:+--type $type}"
# shellcheck disable=SC2086
plan=$("$RIAKCTL" --compact restore --dry-run $opts "$file")
echo "plan:     $(echo "$plan" | jq -r '"\(.would_restore) to write, \(.unchanged) unchanged, \(.skipped) skipped"')"
# shellcheck disable=SC2086
result=$("$RIAKCTL" --compact restore $opts "$file")
echo "restored: $(echo "$result" | jq -r '"\(.restored) written, \(.unchanged) unchanged, \(.failed) failed"')"
[ "$(echo "$result" | jq .failed)" -eq 0 ]
