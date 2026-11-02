#!/bin/sh
# Paged secondary index query: the matches of an index range are read page
# by page with continuations, and each matching object is fetched. The
# objects are printed as JSON Lines {"key", "term", "object"}.
#   secondary-indexes/query.sh BUCKET INDEX MIN MAX [PAGE_SIZE]
set -eu
RIAKCTL=${RIAKCTL:-riakctl}
bucket=${1:?bucket}
index=${2:?index, e.g. age_int}
min=${3:?min}
max=${4:?max}
page=${5:-100}

cont=
pages=0
total=0
while :; do
    result=$("$RIAKCTL" --compact index --terms --max "$page" ${cont:+--continuation "$cont"} \
             "$bucket" "$index" "$min" "$max")
    pages=$((pages + 1))
    echo "$result" | jq -c '.results[]' | while read -r match; do
        key=$(echo "$match" | jq -r .key)
        "$RIAKCTL" --compact get --json "$bucket" "$key" |
            jq -c --argjson m "$match" '{key: $m.key, term: $m.term, object: .}'
    done
    total=$((total + $(echo "$result" | jq '.results | length')))
    cont=$(echo "$result" | jq -r '.continuation // empty')
    [ -n "$cont" ] || break
done
echo "$total matches in $pages pages" >&2
