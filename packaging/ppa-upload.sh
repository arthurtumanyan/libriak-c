#!/bin/sh
# Upload the source packages of packaging/ppa-build.sh to the PPA:
#   packaging/ppa-upload.sh OUTDIR [LAUNCHPAD_USER [PPA]]
# Launchpad takes uploads over anonymous FTP (as dput does) and accepts them
# by the OpenPGP signature of the .changes file, made with a key registered
# on the account. Each .changes file goes last, after the files it lists,
# in the same FTP session. Versions already in the PPA are skipped, so the
# script can be run again after a failed upload.
set -eu
out=${1:?output directory of ppa-build.sh}
user=${2:-tumanyan}; ppa=${3:-libriak-c}
incoming=ftp://ppa.launchpad.net/~$user/ubuntu/$ppa/
archive=https://api.launchpad.net/devel/~$user/+archive/ubuntu/$ppa
cd "$out"
for changes in *_source.changes; do
    source=$(sed -n 's/^Source: *//p' "$changes")
    version=$(sed -n 's/^Version: *//p' "$changes")
    # any publication (pending, published, superseded) means the version was
    # accepted; Launchpad never accepts it a second time
    count=$(curl -fsS -G "$archive" -d ws.op=getPublishedSources -d exact_match=true \
        -d source_name="$source" --data-urlencode version="$version" |
        sed -n 's/.*"total_size": *\([0-9]*\).*/\1/p')
    if [ "${count:-0}" -gt 0 ]; then
        echo "$source $version is already in the PPA"
        continue
    fi
    files=$(sed -n '/^Files:/,/^[^ ]/s/^ [0-9a-f]\{32\} [0-9]* [^ ]* [^ ]* \(.*\)$/\1/p' "$changes" |
        tr '\n' ',')
    # one FTP session per upload: Launchpad processes the files of a session
    # together, so the files and their .changes travel in a single curl call;
    # --fail-early ends the session at the first error, before the .changes
    # is sent, and a new session starts the upload again
    attempt=1
    until curl -fsS --fail-early -T "{${files}$changes}" "$incoming"; do
        [ "$attempt" -lt 3 ] || { echo "upload of $changes failed" >&2; exit 1; }
        attempt=$((attempt + 1))
        sleep 30
    done
    echo "uploaded $changes"
done
