#!/bin/sh
# ctest riakctl_cli: riakctl against tests/stub_riak.py (HTTP, three nodes):
# options, exit codes, JSON output and a backup/restore round trip.
#   python3 tests/stub_riak.py sh tests/cli/stub.sh RIAKCTL
set -u
R=${1:?riakctl binary}
if ! command -v jq >/dev/null 2>&1; then
    echo "jq not found; skipping" >&2
    exit 77
fi
ports=$(echo "$RIAK_TEST_PORTS" | sed 's/,/ /g')
urls=$(for p in $ports; do printf 'http://127.0.0.1:%s,' "$p"; done)
export RIAK_URL="${urls%,}"
unset RIAK_USER RIAK_PASSWORD RIAK_HTTP_PORT RIAK_CA RIAK_CERT RIAK_KEY RIAK_TLS_LEGACY
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0

# expect CODE COMMAND...: the exit code of COMMAND is CODE
expect() {
    want=$1
    shift
    "$@" >"$work/out" 2>"$work/err"
    got=$?
    if [ "$got" -ne "$want" ]; then
        echo "FAIL: '$*' exited with $got, expected $want" >&2
        sed 's/^/  /' "$work/err" >&2
        failures=$((failures + 1))
    fi
}
# check DESCRIPTION CONDITION...
check() {
    what=$1
    shift
    if ! "$@"; then
        echo "FAIL: $what" >&2
        failures=$((failures + 1))
    fi
}

# usage and help
expect 0 "$R" --version
check "version output" grep -q '^riakctl (libriak-c) ' "$work/out"
expect 0 "$R" help
expect 0 "$R" help backup
expect 0 "$R" get --help
expect 2 "$R"
expect 2 "$R" no-such-command
expect 2 "$R" get onlybucket
expect 2 "$R" -u ftp://127.0.0.1 ping
expect 2 "$R" -u 'http://[::1' ping
expect 2 "$R" --user alice ping
RIAK_PASSWORD=x expect 2 "$R" --user alice ping
expect 2 "$R" put --w many b k
expect 2 "$R" dt get b k

# ping: all three nodes, then one closed port
expect 0 "$R" --compact ping
check "three nodes up" [ "$(jq .up "$work/out")" -eq 3 ]
expect 7 "$R" -u http://127.0.0.1:1 --connect-timeout 1000 ping
expect 0 "$R" --compact -u "$RIAK_URL,http://127.0.0.1:1" ping
expect 7 "$R" --compact -u "$RIAK_URL,http://127.0.0.1:1" ping --all

# objects
echo hello | expect 0 "$R" put -c text/plain -m owner=alice -i email_bin=a@b.c -i age_int=30 users u1
expect 0 "$R" get users u1
check "raw value" [ "$(cat "$work/out")" = hello ]
expect 0 "$R" --compact get --json users u1
check "metadata" [ "$(jq -r .meta.owner "$work/out")" = alice ]
check "indexes" [ "$(jq -cS .indexes "$work/out")" = '{"age_int":["30"],"email_bin":["a@b.c"]}' ]
check "content type" [ "$(jq -r .content_type "$work/out")" = text/plain ]
expect 0 "$R" --compact put --value generated users
check "generated key" jq -e '.key | length > 0' "$work/out"
printf '\001\002\377' | expect 0 "$R" put users bin
expect 0 "$R" --compact get --json users bin
check "binary value as base64" [ "$(jq -r .value_b64 "$work/out")" = AQL/ ]
expect 3 "$R" get users missing
expect 4 "$R" get siblings any
printf a | "$R" put -t multi sib k
printf b | "$R" put -t multi sib k
expect 4 "$R" get -t multi sib k
expect 0 "$R" --compact get --all -t multi sib k
check "siblings listed" [ "$(jq -c '[.siblings[].value] | sort' "$work/out")" = '["a","b"]' ]
expect 0 "$R" delete users u1
expect 3 "$R" get users u1
expect 0 "$R" --compact list keys users
check "key listing as JSON Lines" jq -e 'type == "string"' "$work/out"
expect 0 "$R" --compact props get users
check "bucket properties" jq -e '.props.n_val == 3' "$work/out"

# backup and restore (key listing over HTTP), then a second, idempotent restore
for i in 1 2 3; do
    printf 'v%s' "$i" | "$R" put -i "n_int=$i" -m "m=$i" src "k$i" || { echo "FAIL: put k$i" >&2; failures=$((failures + 1)); }
done
expect 0 "$R" backup --method keys -o "$work/b.jsonl" src
check "backup trailer" [ "$(tail -n 1 "$work/b.jsonl" | jq .count)" -eq 3 ]
expect 0 "$R" --compact restore -b dst "$work/b.jsonl"
check "restored" [ "$(jq .restored "$work/out")" -eq 3 ]
expect 0 "$R" --compact restore -b dst "$work/b.jsonl"
check "second restore unchanged" [ "$(jq .unchanged "$work/out")" -eq 3 ]
expect 0 "$R" --compact get --json dst k2
check "restored object" [ "$(jq -c '[.value, .meta.m, .indexes.n_int[0]]' "$work/out")" = '["v2","2","2"]' ]
# parallel restore: the same result with 8 workers
expect 0 "$R" --compact restore --jobs 8 -b dst8 "$work/b.jsonl"
check "parallel restore" [ "$(jq -c '[.restored, .failed, .complete]' "$work/out")" = '[3,0,true]' ]
expect 0 "$R" --compact get --json dst8 k3
check "parallel restored object" [ "$(jq -c '[.value, .meta.m]' "$work/out")" = '["v3","3"]' ]
expect 2 "$R" restore --jobs 0 "$work/b.jsonl"
expect 2 "$R" restore --jobs 65 "$work/b.jsonl"

# siblings into a bucket without allow_mult: the last modified one is kept,
# and a second restore finds it unchanged
cat >"$work/sib.jsonl" <<'JSONL'
{"format":"riakctl-backup","version":1,"bucket":"plain"}
{"key":"s","siblings":[{"value":"old","last_modified":"Mon, 02 Nov 2026 10:00:00 GMT"},{"value":"new","last_modified":"Mon, 02 Nov 2026 11:00:00 GMT"},{"value":"older","last_modified":"Sun, 01 Nov 2026 23:00:00 GMT"}]}
{"end":true,"count":1}
JSONL
expect 0 "$R" --compact restore "$work/sib.jsonl"
check "collapsed" [ "$(jq -c '[.restored, .collapsed]' "$work/out")" = '[1,1]' ]
check "collapse warning" grep -q 'does not keep siblings' "$work/err"
expect 0 "$R" get plain s
check "newest sibling kept" [ "$(cat "$work/out")" = new ]
expect 0 "$R" --compact restore "$work/sib.jsonl"
check "collapsed key unchanged" [ "$(jq -c '[.restored, .unchanged]' "$work/out")" = '[0,1]' ]

head -n 2 "$work/b.jsonl" >"$work/cut.jsonl"
expect 1 "$R" restore -b dst2 "$work/cut.jsonl"
echo '{"format":"other"}' >"$work/bad.jsonl"
expect 1 "$R" restore "$work/bad.jsonl"

# AAE folds (canned answers of the stub)
expect 0 "$R" --compact aae stats b
check "aae stats" [ "$(jq .total_count "$work/out")" -eq 12 ]
expect 0 "$R" --compact aae reap b
check "aae reap counts" [ "$(jq -c . "$work/out")" = '{"mode":"count","count":7}' ]
expect 0 "$R" --compact aae buckets 3
check "aae buckets" [ "$(wc -l <"$work/out")" -eq 2 ]
expect 2 "$R" aae find b
expect 2 "$R" aae tree b --size huge

if [ "$failures" -ne 0 ]; then
    echo "$failures failures" >&2
    exit 1
fi
echo "riakctl stub tests passed"
