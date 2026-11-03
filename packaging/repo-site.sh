#!/bin/sh
# Build the package repository site from release packages:
# packaging/repo-site.sh ASSETS SITE KEY URL
# The .deb and .rpm files of ASSETS are sorted by distribution into
#   SITE/deb/ubuntu-<version>/   flat apt repositories
#   SITE/rpm/fedora/<version>/   dnf repositories
#   SITE/rpm/el/<version>/       dnf repositories (AlmaLinux, Rocky, RHEL)
# and indexed and signed with the gpg key KEY by make-repo.sh. The public
# key and the dnf repository definitions for the site at URL are added.
set -eu
assets=${1:?assets directory}
site=${2:?site directory}
key=${3:?signing key}
url=${4:?site URL}
here=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$site"
for f in "$assets"/*.deb "$assets"/*.rpm; do
    [ -e "$f" ] || continue
    name=$(basename "$f")
    case $name in
        *[.~]ubuntu[0-9]*_*.deb)
            dist=deb/ubuntu-$(echo "$name" | sed 's/.*[.~]ubuntu\([0-9.]*\)_.*/\1/') ;;
        *.fc[0-9]*.rpm)
            dist=rpm/fedora/$(echo "$name" | sed 's/.*\.fc\([0-9]*\)\..*/\1/') ;;
        *.el[0-9]*.rpm)
            dist=rpm/el/$(echo "$name" | sed 's/.*\.el\([0-9]*\)\..*/\1/') ;;
        *)
            echo "$name: distribution not recognised" >&2
            exit 1 ;;
    esac
    mkdir -p "$site/$dist"
    cp "$f" "$site/$dist/"
done

dirs=$(find "$site/deb" "$site/rpm" -mindepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) \
    -exec dirname {} \; 2>/dev/null | sort -u)
[ -n "$dirs" ] || { echo "no packages in $assets" >&2; exit 1; }
# shellcheck disable=SC2086 # one directory per word
"$here/make-repo.sh" "$key" $dirs

gpg --batch --yes --armor --export "$key" > "$site/libriak-c.asc"
for family in fedora el; do
    [ -d "$site/rpm/$family" ] || continue
    cat > "$site/libriak-c-$family.repo" <<EOF
[libriak-c]
name=libriak-c
baseurl=$url/rpm/$family/\$releasever/
enabled=1
repo_gpgcheck=1
gpgcheck=0
gpgkey=$url/libriak-c.asc
EOF
done
touch "$site/.nojekyll"
