#!/bin/sh
# Runs the demos as one workflow against the node(s) of RIAK_URL and checks
# their results: connectivity, seeding, backup, a comparison with another
# cluster, wiping the bucket, restore (twice), a paged index query,
# monitoring, AAE inspection and a replication queue round trip. Every key written lives in a fresh bucket
# that is removed at the end.
#   RIAK_URL=pb://127.0.0.1:8087 examples/cli/run-all.sh
# RIAK_FEATURES (comma separated: 2i, aae, nextgenrepl) skips the detection
# of optional features; RIAK_REPL_QUEUE names the nextgenrepl source queue
# (default libriak_q); RIAK_MULTI_TYPE and RIAK_MAP_TYPE name an allow_mult
# and a map bucket type (default libriak_multi and libriak_maps, as created
# by tests/docker/setup-node.sh); missing ones are skipped. RIAK_PEER_URL
# names a second cluster (same credentials) for the comparison of buckets
# restored there; without it the comparison is skipped.
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
copy=$bucket-copy
# the peer cluster of the comparison (RIAK_PEER_URL), same credentials
rp() { RIAK_URL=$RIAK_PEER_URL r "$@"; }
cleanup() {
    for t in "" "$multi"; do
        r ${t:+--type "$t"} list --plain keys "$bucket" 2>/dev/null | while read -r k; do
            r ${t:+--type "$t"} delete "$bucket" "$k" || true
        done
    done
    if [ -n "${RIAK_PEER_URL:-}" ]; then
        for b in "$bucket" "$copy"; do
            rp list --plain keys "$b" 2>/dev/null | while read -r k; do
                rp delete "$b" "$k" || true
            done
        done
    fi
    rm -rf "$work"
}
trap cleanup EXIT

features=${RIAK_FEATURES:-}
if [ -z "$features" ]; then
    # in a bucket of its own: its tombstone stays in the demo bucket's
    # AAE results for a few seconds otherwise
    printf 1 | r put -i probe_int=1 "$bucket-probe" probe
    r index "$bucket-probe" probe_int 1 >/dev/null 2>&1 && features=2i
    r aae buckets 3 >/dev/null 2>&1 && features="$features,aae"
    r repl fetch "$queue" --max 1 >/dev/null 2>&1 && features="$features,nextgenrepl"
    r delete "$bucket-probe" probe
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

if has aae && [ -z "${RIAK_PEER_URL:-}" ]; then
    echo "no RIAK_PEER_URL: comparison with another cluster skipped"
elif has aae; then
    # Another cluster, as for a migration, and the bucket as backed up,
    # before it is wiped and restored here: restored "as is" into its own
    # cluster, a key's clock can get an entry (a vnode that receives a
    # clock holding its own actor for a key it does not store starts a new
    # key epoch) on one replica first, until anti-entropy repairs the
    # others, so the clocks would not be a stable reference.
    step "comparison with the cluster of $RIAK_PEER_URL"
    RIAK_URL=$RIAK_PEER_URL "$here/basic/restore.sh" "$work/objects.jsonl" >/dev/null
    PEER_URL=$RIAK_PEER_URL "$here/operations/bucket-compare.sh" "$bucket" "$bucket" ||
        fail "the peer differs after the restore"
    RIAK_URL=$RIAK_PEER_URL "$here/basic/restore.sh" "$work/objects.jsonl" "$copy" >/dev/null
    printf changed | rp put "$copy" user1
    printf extra | rp put "$copy" only_in_copy
    printf extra | r put "$bucket" only_in_original
    # ranges of 10 keys: the 27 keys of each bucket take several folds
    rc=0
    CHUNK=10 PEER_URL=$RIAK_PEER_URL "$here/operations/bucket-compare.sh" "$bucket" "$copy" \
        >"$work/compare.txt" || rc=$?
    cat "$work/compare.txt"
    [ "$rc" -eq 1 ] || fail "bucket comparison exit code $rc"
    [ "$(grep -v ' differ in ' "$work/compare.txt")" = "differs user1
missing_locally only_in_copy
missing_on_peer only_in_original" ] || fail "unexpected comparison result"
    r delete "$bucket" only_in_original
fi

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
    # the first sibling replaces the tombstone of the wiped key
    [ "$(r --type "$multi" get --all "$bucket" conflict |
         jq -c '[.siblings[] | .value] | sort')" = '["left","right"]' ] ||
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
# the latencies depend on the machine, so the classification is checked
# with thresholds that every node is below (OK) or above (CRITICAL)
check() {
    rc=0
    "$here/automation/health-check.sh" "$1" "$2" >"$work/check.txt" || rc=$?
    cat "$work/check.txt"
    [ "$rc" -eq "$3" ] || fail "health check exit code $rc, expected $3"
    grep -q "^$4 - .* | get95=[0-9]*us;$1;$2 put95=[0-9]*us;$1;$2 nodes_up=[0-9]*;;;0;[0-9]*\$" \
        "$work/check.txt" || fail "health check output"
}
check 1000000000 2000000000 0 OK
check 0 0 2 CRITICAL
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
