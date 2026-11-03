#!/bin/sh
# Bucket inspection through AAE folds (Riak 2.9+ with tictac AAE), without
# reading any object: object count and size, keys with too many siblings or
# too large values, and tombstones waiting to be reaped. With --reap the
# tombstones are handed to the nodes' reapers.
#   operations/aae-inspection.sh [--reap] BUCKET [BUCKET_TYPE]
set -eu
RIAKCTL=${RIAKCTL:-riakctl}
reap=
if [ "${1:-}" = --reap ]; then
    reap=1
    shift
fi
bucket=${1:?bucket}
type=${2:-}
SIBLINGS=${SIBLINGS:-5}
LARGE=${LARGE:-1048576}
r() { "$RIAKCTL" --compact ${type:+--type "$type"} "$@"; }

r aae stats "$bucket" | jq -r '"objects: \(.total_count), \(.total_size) bytes",
    (.siblings[] | "  \(.count) objects with \(.siblings) sibling(s)")'
echo "keys with more than $SIBLINGS siblings:"
r aae find "$bucket" --siblings "$SIBLINGS" | jq -r '"  \(.key) (\(.siblings))"'
echo "keys larger than $LARGE bytes:"
r aae find "$bucket" --object-size "$LARGE" | jq -r '"  \(.key) (\(.size) bytes)"'
tombs=$(r aae reap "$bucket" | jq .count)
echo "tombstones: $tombs"
if [ -n "$reap" ] && [ "$tombs" -gt 0 ]; then
    r aae reap --local "$bucket" | jq -r '"reaping \(.count) tombstones"'
fi
