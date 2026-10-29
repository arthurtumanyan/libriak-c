#!/bin/sh
# tests/abi/check_symbols.sh <libriak-c.so> <symbols-file>
# Every function of the list is exported under LIBRIAK_2.0 (as the only
# definition, or as the 2.0 entry point next to a newer default); every
# exported function carries a LIBRIAK_ version; nothing else is exported.
set -eu
lib=${1:?shared library}
list=${2:?symbol list}
exports=$(objdump -T "$lib" | awk '$4 == ".text" { print $(NF-1), $NF }')
status=0
for name in $(grep -v '^#' "$list"); do
    if ! printf '%s\n' "$exports" | grep -Eq "^\(?LIBRIAK_2\.0\)? $name\$"; then
        echo "missing: $name@LIBRIAK_2.0"
        status=1
    fi
done
unversioned=$(printf '%s\n' "$exports" | grep -Ev '^\(?LIBRIAK_[0-9.]+\)? ' || true)
if [ -n "$unversioned" ]; then
    echo "exported without a version:"
    printf '%s\n' "$unversioned"
    status=1
fi
foreign=$(printf '%s\n' "$exports" | awk '$2 !~ /^riak_/' || true)
if [ -n "$foreign" ]; then
    echo "exported but not public:"
    printf '%s\n' "$foreign"
    status=1
fi
n=$(grep -cv '^#' "$list")
[ $status -eq 0 ] && echo "$n functions of 2.0.0 exported under LIBRIAK_2.0"
exit $status
