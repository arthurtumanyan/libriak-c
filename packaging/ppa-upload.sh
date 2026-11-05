#!/bin/sh
# Upload the source packages of packaging/ppa-build.sh to the PPA:
#   packaging/ppa-upload.sh OUTDIR [LAUNCHPAD_USER [PPA]]
# Launchpad takes uploads over anonymous FTP (as dput does) and accepts them
# by the OpenPGP signature of the .changes file, made with a key registered
# on the account. Each .changes file goes last, after the files it lists,
# in the same FTP session.
set -eu
out=${1:?output directory of ppa-build.sh}
user=${2:-tumanyan}; ppa=${3:-libriak-c}
incoming=ftp://ppa.launchpad.net/~$user/ubuntu/$ppa/
cd "$out"
for changes in *_source.changes; do
    files=$(sed -n '/^Files:/,/^[^ ]/s/^ [0-9a-f]\{32\} [0-9]* [^ ]* [^ ]* \(.*\)$/\1/p' "$changes" |
        tr '\n' ',')
    # one FTP session per upload: Launchpad processes the files of a session
    # together, so the files and their .changes travel in a single curl call
    curl -fsS -T "{${files}$changes}" "$incoming"
    echo "uploaded $changes"
done
