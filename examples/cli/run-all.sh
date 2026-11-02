#!/bin/sh
# Runs the demos as one workflow against the node(s) of RIAK_URL and checks
# their results: connectivity, seeding, backup, wiping the bucket, restore
# (twice), a paged index query, monitoring, AAE inspection and a
# replication queue round trip. Every key written lives in a fresh bucket
# that is removed at the end.
#   RIAK_URL=pb://127.0.0.1:8087 examples/cli/run-all.sh
# RIAK_FEATURES (comma separated: 2i, aae, nextgenrepl) skips the detection
# of optional features; RIAK_REPL_QUEUE names the nextgenrepl source queue
# (default libriak_q); RIAK_MULTI_TYPE and RIAK_MAP_TYPE name an allow_mult
# and a map bucket type (default libriak_multi and libriak_maps, as created
# by tests/docker/setup-node.sh); missing ones are skipped.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
RIAKCTL=${RIAKCTL:-riakctl}
export RIAKCTL
queue=${RIAK_REPL_QUEUE:-libriak_q}
multi=${RIAK_MULTI_TYPE:-libriak_multi}
maps=${RIAK_MAP_TYPE:-libriak_maps}
work=$(mktemp -d)
bucket=riakctl-demo-$(date +%s)-$$
r() { "$RIAKCTL" --compact "$@"; }
step() { printf '\n== %s\n' "$*"; }
fail() { echo "FAILED: $*" >&2; exit 1; }
cleanup() {
    for t in "" "$multi"; do
        r ${t:+--type "$t"} list --plain keys "$bucket" 2>/dev/null | while read -r k; do
            r ${t:+--type "$t"} delete "$bucket" "$k" || true
        done
    done
    rm -rf "$work"
}
trap cleanup EXIT

features=${RIAK_FEATURES:-}
if [ -z "$features" ]; then
    printf 1 | r put -i probe_int=1 "$bucket" probe
    r index "$bucket" probe_int 1 >/dev/null 2>&1 && features=2i
    r aae buckets 3 >/dev/null 2>&1 && features="$features,aae"
    r repl fetch "$queue" --max 1 >/dev/null 2>&1 && features="$features,nextgenrepl"
    r delete "$bucket" probe
fi
has() { case ",$features," in *",$1,"*) return 0 ;; esac; return 1; }
has_type() { r props type-get "$1" >/dev/null 2>&1; }
echo "bucket $bucket, features: ${features:-none}"

step connectivity
"$here/basic/connectivity.sh"

step "seeding $bucket"
for i in $(seq 1 25); do
    printf '{"id":%d,"name":"user%d"}' "$i" "$i" |
        r put -c application/json -m source=demo -i "age_int=$((20 + i))" -i "name_bin=user$i" \
            "$bucket" "user$i"
done
head -c 256 /dev/urandom >"$work/blob"
r put -f "$work/blob" -c application/octet-stream "$bucket" blob
echo "26 objects written"
if has_type "$multi"; then
    printf left | r --type "$multi" put "$bucket" conflict
    printf right | r --type "$multi" put "$bucket" conflict
    [ "$(r --type "$multi" get --all "$bucket" conflict | jq '.siblings | length')" -eq 2 ] ||
        fail "no siblings in $multi"
    echo "1 key with 2 siblings in bucket type $multi"
fi
if has_type "$maps"; then
    echo '{"name_register":"demo","visits_counter":3,"tags_set":{"add":["a","b"]}}' |
        r --type "$maps" dt update "$bucket" profile
    echo "1 map in bucket type $maps"
fi

step backup
"$here/basic/backup.sh" "$bucket" "$work/objects.jsonl"
has_type "$multi" && "$here/basic/backup.sh" "$bucket" "$work/siblings.jsonl" "$multi"
has_type "$maps" && "$here/basic/backup.sh" "$bucket" "$work/maps.jsonl" "$maps"

step "wiping $bucket"
r list --plain keys "$bucket" | while read -r k; do r delete "$bucket" "$k"; done
has_type "$multi" && r --type "$multi" delete "$bucket" conflict
has_type "$maps" && r --type "$maps" dt update "$bucket" profile <<'JSON'
{"remove": ["name_register", "visits_counter", "tags_set"]}
JSON
rc=0
r get "$bucket" user1 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 3 ] || fail "user1 still readable after the wipe (exit code $rc)"
echo "bucket emptied"

step restore
"$here/basic/restore.sh" "$work/objects.jsonl"
has_type "$multi" && "$here/basic/restore.sh" "$work/siblings.jsonl" "" "$multi"
has_type "$maps" && "$here/basic/restore.sh" "$work/maps.jsonl" "" "$maps"
# the restored bucket matches the backup (last-modified times and vector
# clocks change: the keys were deleted in between)
strip() { jq -c 'select(.key) | del(.vclock, .siblings[]?.last_modified)' "$1" | sort; }
"$RIAKCTL" backup -o "$work/again.jsonl" "$bucket" 2>/dev/null
[ "$(strip "$work/objects.jsonl")" = "$(strip "$work/again.jsonl")" ] || fail "restored objects differ"
r get "$bucket" blob | cmp -s "$work/blob" - || fail "binary value differs"
if has_type "$multi"; then
    # over HTTP the tombstone of the wiped key can remain as one more sibling
    [ "$(r --type "$multi" get --all "$bucket" conflict |
         jq -c '[.siblings[] | select(.deleted | not) | .value] | sort')" = '["left","right"]' ] ||
        fail "siblings not restored"
fi
if has_type "$maps"; then
    [ "$(r --type "$maps" dt get "$bucket" profile | jq -c '.value | .tags_set |= sort')" = \
      '{"name_register":"demo","tags_set":["a","b"],"visits_counter":3}' ] || fail "map not restored"
fi
echo "objects, siblings and data types identical to the backup"
again=$(r restore "$work/objects.jsonl")
[ "$(echo "$again" | jq .restored)" -eq 0 ] || fail "second restore wrote objects: $again"
echo "second restore: $(echo "$again" | jq .unchanged) unchanged"

if has 2i; then
    step "paged index query"
    "$here/secondary-indexes/query.sh" "$bucket" age_int 31 40 4 >"$work/matches.jsonl"
    [ "$(wc -l <"$work/matches.jsonl")" -eq 10 ] || fail "expected 10 index matches"
    jq -r '"\(.key)\tage \(.term)\t\(.object.value)"' "$work/matches.jsonl" | head -3
fi

step monitoring
rc=0
"$here/automation/health-check.sh" || rc=$?
[ "$rc" -le 1 ] || fail "health check exit code $rc"
"$here/automation/cluster-stats.sh" 1 2 >"$work/stats.csv"
[ "$(wc -l <"$work/stats.csv")" -eq 3 ] || fail "expected a header and 2 samples"
cat "$work/stats.csv"

if has aae; then
    step "AAE inspection"
    "$here/operations/aae-inspection.sh" "$bucket"
fi
# queue pushes need Protocol Buffers
case ${RIAK_URL:-pb://} in
pb*) ;;
*) features=$(echo "$features" | sed 's/nextgenrepl//') ;;
esac
if has nextgenrepl; then
    step "replication queue $queue"
    "$here/operations/replication-queues.sh" "$bucket" "$queue"
fi

printf '\nall demos passed\n'
