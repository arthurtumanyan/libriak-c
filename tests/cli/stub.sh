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

# a password file readable by others is reported; the password is used
printf 'secret\nsecond line\n' >"$work/pw"
chmod 644 "$work/pw"
expect 2 "$R" -u "http://127.0.0.1:${RIAK_TEST_PORT}" --user alice --password-file "$work/pw" ping
check "password file permissions reported" grep -q 'can be read by other users' "$work/err"
chmod 600 "$work/pw"
expect 2 "$R" -u "http://127.0.0.1:${RIAK_TEST_PORT}" --user alice --password-file "$work/pw" ping
check "no warning for a private password file" sh -c "! grep -q 'can be read by other users' '$work/err'"

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
check "generated key" jq -e '.key | length > 0' "$work/out" >/dev/null
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
check "key listing as JSON Lines" jq -e 'type == "string"' "$work/out" >/dev/null
expect 0 "$R" --compact props get users
check "bucket properties" jq -e '.props.n_val == 3' "$work/out" >/dev/null

# backup and restore (key listing over HTTP), then a second, idempotent restore
for i in 1 2 3; do
    printf 'v%s' "$i" | "$R" put -i "n_int=$i" -m "m=$i" src "k$i" || { echo "FAIL: put k$i" >&2; failures=$((failures + 1)); }
done
expect 0 "$R" backup --method keys -o "$work/b.jsonl" src
check "backup file only readable by its owner" [ "$(stat -c %a "$work/b.jsonl")" = 600 ]
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

# A request straight to the first stub node: stub_http METHOD PATH
first=${ports%% *}
stub_http() {
    python3 -c 'import sys, urllib.request
req = urllib.request.Request("http://127.0.0.1:" + sys.argv[1] + sys.argv[3], method=sys.argv[2],
                             data=b"x" if sys.argv[2] == "PUT" else None,
                             headers={"Content-Type": "text/plain"})
try:
    sys.stdout.write(urllib.request.urlopen(req).read().decode())
except urllib.error.HTTPError as e:
    sys.stdout.write(e.read().decode())' "$first" "$@"
}

# restore over a deleted key: the write descends the tombstone's clock,
# which comes with the 404 over HTTP too
printf 'live' | "$R" -u "http://127.0.0.1:$first" put tb k || failures=$((failures + 1))
expect 0 "$R" -u "http://127.0.0.1:$first" backup --method keys -o "$work/tb.jsonl" tb
expect 0 "$R" -u "http://127.0.0.1:$first" delete tb k
expect 0 "$R" -u "http://127.0.0.1:$first" --compact restore "$work/tb.jsonl"
check "restored over a tombstone" [ "$(jq -c '[.restored, .failed]' "$work/out")" = '[1,0]' ]
stub_http GET /_last >"$work/last"
check "written with the tombstone's clock" \
    [ "$(jq -r '.headers["X-Riak-Vclock"] // "" | startswith("vc-tomb-")' "$work/last")" = true ]
expect 0 "$R" -u "http://127.0.0.1:$first" get tb k
check "value back after the restore" [ "$(cat "$work/out")" = live ]

# entries longer than --max-line are skipped and counted as failed; the
# others are restored
big=$(head -c 3000 /dev/zero | tr '\0' 'x')
cat >"$work/long.jsonl" <<JSONL
{"format":"riakctl-backup","version":1,"bucket":"longb"}
{"key":"small","siblings":[{"value":"s"}]}
{"key":"big","siblings":[{"value":"$big"}]}
{"key":"small2","siblings":[{"value":"s2"}]}
{"end":true,"count":3}
JSONL
expect 1 "$R" --compact restore --max-line 2048 "$work/long.jsonl"
check "long entry failed, others restored" \
    [ "$(jq -c '[.restored, .failed, .complete]' "$work/out")" = '[2,1,true]' ]
check "long entry reported" grep -q 'line 3: 30[0-9][0-9] bytes, longer than --max-line 2048' "$work/err"
expect 0 "$R" --compact restore "$work/long.jsonl"
check "restored with the default limit" [ "$(jq -c '[.restored, .unchanged]' "$work/out")" = '[1,2]' ]
expect 2 "$R" restore --max-line 1000 "$work/long.jsonl"
expect 2 "$R" restore --max-line 12x "$work/long.jsonl"
# a header longer than 1 MiB is not a backup
{ printf '{"format":"riakctl-backup","pad":"'; head -c 1100000 /dev/zero | tr '\0' 'p'; printf '"}\n'; } >"$work/hdr.jsonl"
expect 1 "$R" restore "$work/hdr.jsonl"
check "long header reported" grep -q 'header too long' "$work/err"

# a key with a NUL byte cannot be written: the entry fails, the rest is restored
cat >"$work/nul.jsonl" <<'JSONL'
{"format":"riakctl-backup","version":1,"bucket":"nulb"}
{"key":"a\u0000b","siblings":[{"value":"x"}]}
{"key":"ok","siblings":[{"value":"y"}]}
{"end":true,"count":2}
JSONL
expect 1 "$R" --compact restore "$work/nul.jsonl"
check "NUL key entry failed" [ "$(jq -c '[.restored, .failed]' "$work/out")" = '[1,1]' ]
check "NUL key reported" grep -q 'not a string without NUL bytes' "$work/err"
expect 3 "$R" get nulb a

# keys with a NUL byte from the server are left out, reported, and make the
# result (exit status 1) and a backup (trailer "left_out") incomplete
stub_http PUT '/buckets/nulsrc/keys/a%00b' >/dev/null
printf 'v' | "$R" put nulsrc plain || failures=$((failures + 1))
expect 1 "$R" --compact list keys nulsrc
check "NUL key left out of the listing" [ "$(jq -sc . "$work/out")" = '["plain"]' ]
check "left out key reported" grep -q '1 key(s) or bucket name(s) with a NUL byte were left out' "$work/err"
expect 1 "$R" backup --method keys -o "$work/nul-b.jsonl" nulsrc
check "backup trailer counts the left out key" \
    [ "$(tail -n 1 "$work/nul-b.jsonl" | jq -c '[.count, .left_out]')" = '[1,1]' ]

# two buckets compared in key ranges paged from $bucket: every key once,
# with the right status, also a tombstone after the last stored key, and no
# fold returns more than a page of keys per side
i=0
while [ $i -lt 180 ]; do
    k=$(printf 'k%03d' $i)
    if [ $i -lt 120 ]; then printf 'a' | "$R" put cmpa "$k" || failures=$((failures + 1)); fi
    if [ $i -ge 60 ]; then printf 'b' | "$R" put cmpb "$k" || failures=$((failures + 1)); fi
    i=$((i + 1))
done
printf 't' | "$R" put cmpa z-tomb && "$R" delete cmpa z-tomb >/dev/null || failures=$((failures + 1))
printf 'm' | "$R" put cmpa k0300 && "$R" delete cmpa k0300 >/dev/null || failures=$((failures + 1))
stub_http GET /_folds >/dev/null
expect 0 "$R" --compact aae compare cmpa --peer-bucket cmpb --chunk 10
jq -sc 'group_by(.status) | map({(.[0].status): length}) | add' "$work/out" >"$work/counts"
check "compare counts" [ "$(cat "$work/counts")" = '{"differs":60,"missing_locally":60,"missing_on_peer":62}' ]
check "every key once" [ "$(jq -s 'map(.key) | (length == (unique | length))' "$work/out")" = true ]
check "tombstone after the last key compared" [ "$(jq -c 'select(.key == "z-tomb") | .status' "$work/out")" = '"missing_on_peer"' ]
check "status by key" [ "$(jq -sc 'map(select(.key == "k059" or .key == "k060" or .key == "k119" or .key == "k120")) | sort_by(.key) | map(.status)' "$work/out")" = '["missing_on_peer","differs","differs","missing_locally"]' ]
check "compare in ranges" grep -q 'key by key, [0-9][0-9]* ranges' "$work/err"
folds=$(stub_http GET /_folds)
check "folds bounded by the chunk ($folds)" [ "$(echo "$folds" | jq '.max <= 12 and .count >= 24')" = true ]
# the same result in one range
expect 0 "$R" --compact aae compare cmpa --peer-bucket cmpb
check "compare with the default chunk" [ "$(jq -s length "$work/out")" -eq 182 ]
# without secondary indexes the lists are compared whole, with a warning
expect 0 "$R" --compact aae compare unavailable --peer-bucket cmpb
check "fallback without 2i" grep -q 'compared whole, in memory' "$work/err"
expect 2 "$R" aae compare cmpa --peer-bucket cmpb --chunk 5

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
