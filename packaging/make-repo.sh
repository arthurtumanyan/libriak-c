#!/bin/sh
# Turn directories of prebuilt packages into signed apt or dnf repositories:
# packaging/make-repo.sh KEY DIR...
# A directory of .deb files becomes a flat apt repository (Packages, Release,
# InRelease, Release.gpg), a directory of .rpm files a dnf repository
# (repodata/ with a detached signature of repomd.xml). The indexes are
# signed with the gpg key KEY. Needs apt-ftparchive, createrepo_c and gpg.
set -eu
key=${1:?signing key}
shift
[ $# -gt 0 ] || { echo "usage: $0 KEY DIR..." >&2; exit 2; }

for dir in "$@"; do
    [ -d "$dir" ] || { echo "$dir: not a directory" >&2; exit 1; }
    if ls "$dir"/*.deb >/dev/null 2>&1; then
        (
            cd "$dir"
            rm -f Packages Packages.gz Release InRelease Release.gpg
            apt-ftparchive packages . > Packages
            gzip -9nk Packages
            apt-ftparchive \
                -o APT::FTPArchive::Release::Origin=libriak-c \
                -o APT::FTPArchive::Release::Label=libriak-c \
                -o "APT::FTPArchive::Release::Suite=$(basename "$PWD")" \
                -o APT::FTPArchive::Release::Architectures=amd64 \
                release . > Release.tmp
            mv Release.tmp Release
            gpg --batch --yes -u "$key" --clearsign -o InRelease Release
            gpg --batch --yes -u "$key" -abs -o Release.gpg Release
        )
    elif ls "$dir"/*.rpm >/dev/null 2>&1; then
        rm -rf "$dir/repodata"
        createrepo_c --quiet --general-compress-type=gz "$dir"
        gpg --batch --yes -u "$key" -ab -o "$dir/repodata/repomd.xml.asc" \
            "$dir/repodata/repomd.xml"
    else
        echo "$dir: no .deb or .rpm files" >&2
        exit 1
    fi
    echo "$dir: repository written"
done
