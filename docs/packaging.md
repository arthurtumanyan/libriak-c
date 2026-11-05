# Packaging and releases

## Packages

Each format consists of a runtime, a development and a tools package:

| | Debian / Ubuntu | Fedora / EL |
|---|---|---|
| Runtime | `libriak-c2` | `libriak-c` |
| Development | `libriak-c-dev` | `libriak-c-devel` |
| Tools | `riakctl` | `riakctl` |

The runtime package holds the shared library and documentation; the
development package holds the header, the static library, the `.so` link,
the CMake package and the pkg-config file. The tools package holds the
[`riakctl`](riakctl.md) command-line client and its man page, and depends on
the runtime package of the same version. Runtime dependencies are derived
by `dpkg-shlibdeps` and RPM autoreq; the development packages depend on the
libcurl, json-c, protobuf-c, OpenSSL and zlib development packages.

Up to 2.2.1 the tools package was named `libriak-c-tools`. From 2.2.2 on,
the `riakctl` package obsoletes it on Fedora/EL (`dnf upgrade` replaces it);
on Debian/Ubuntu a transitional `libriak-c-tools` package depends on
`riakctl`, so `apt upgrade` installs `riakctl`; the transitional package can
then be removed with `apt remove libriak-c-tools`.

The runtime package of every 2.x release is named after the library's
major version (`libriak-c2`, `libriak-c.so.2`): a 2.1 package replaces 2.0,
and programs built against 2.0.0 keep working with it (see
[Migrating](migration.md)).

Package versions carry the distribution (`2.1.0-1~ubuntu24.04`,
`2.1.0-1.fc42`), so packages for different distributions do not collide.
They are built and install-tested on Ubuntu 22.04, 24.04 and 26.04, Debian
12 and 13, Fedora 44 and 45 and AlmaLinux 9 and 10.

Packages are built locally with:

```sh
packaging/build-packages.sh DEB      # or RPM; output in dist/
```

`packaging/smoke-test.sh` compiles `examples/basic.c` against the installed
packages through pkg-config and through the CMake package, shared and static,
and checks the installed `riakctl` (version, linkage, exit code of an
unreachable node, packaged man page).

## Package repository

Signed apt and dnf repositories with the packages of all releases are
published on GitHub Pages at https://arthurtumanyan.github.io/libriak-c/.
With the repository configured, the package manager resolves the
dependencies itself: installing `riakctl` or `libriak-c-dev` pulls in
the runtime package of the same version, and upgrades arrive with the
regular system updates.

Ubuntu 22.04, 24.04 and 26.04 and Debian 12 and 13 (the directory names
the distribution release: `ubuntu-22.04`, `ubuntu-24.04`, `ubuntu-26.04`,
`debian-12` or `debian-13`):

```sh
repo=https://arthurtumanyan.github.io/libriak-c
sudo install -d -m 755 /etc/apt/keyrings
curl -fsSL "$repo/libriak-c.asc" | sudo tee /etc/apt/keyrings/libriak-c.asc > /dev/null
echo "deb [signed-by=/etc/apt/keyrings/libriak-c.asc] $repo/deb/ubuntu-24.04 ./" |
    sudo tee /etc/apt/sources.list.d/libriak-c.list
sudo apt update
sudo apt install riakctl    # libriak-c-dev for development
```

Fedora 44 and 45 (`libriak-c-fedora.repo`) and AlmaLinux, Rocky Linux or
RHEL 9 and 10 (`libriak-c-el.repo`):

```sh
sudo curl -fsSL -o /etc/yum.repos.d/libriak-c.repo \
    https://arthurtumanyan.github.io/libriak-c/libriak-c-fedora.repo
sudo dnf install riakctl    # libriak-c-devel for development
```

The repository metadata (`InRelease`, `Release.gpg`, `repomd.xml.asc`) is
signed with the release signing key
`D3C2A1C79D9B63A5DAEF362507E49B41B39FBE84`, the key of the release tags; the
packages are covered through the checksums in the signed metadata. dnf asks
to confirm the import of the key on first use.

The layout is `deb/<ubuntu|debian>-<version>/` (flat apt repositories),
`rpm/fedora/<version>/` and `rpm/el/<version>/` (selected through
`$releasever`). A distribution release receives the releases of libriak-c
built for it. Up to 2.2.0 the releases were built for Ubuntu 22.04 and
24.04, the newest Fedora (44 for 2.1.0, 45 for 2.2.0) and AlmaLinux 10;
Ubuntu 26.04, Debian 12 and 13, Fedora 44 and EL 9 receive packages from
2.2.1 on.

## Fedora Copr

The Fedora and EL packages are also built in the Copr project
[`atumanyan/libriak-c`](https://copr.fedorainfracloud.org/coprs/atumanyan/libriak-c/)
for Fedora 44 and 45 and EPEL 9 and 10, from the release tarball and
`packaging/rpm/libriak-c.spec` (same package split and names as above):

```sh
sudo dnf copr enable atumanyan/libriak-c
sudo dnf install riakctl
```

Copr signs the packages with its own per-project key. A published release
is submitted by the release workflow (`.github/workflows/distributions.yml`,
job `copr`), which waits for the Copr build. It needs the copr-cli
configuration with an API token
(https://copr.fedorainfracloud.org/api/) as the repository secret
`COPR_CONFIG`; Copr API tokens expire after 180 days and are renewed on the
same page. By hand:

```sh
gh secret set COPR_CONFIG < ~/.config/copr
packaging/copr-build.sh 2.2.2    # needs copr-cli and ~/.config/copr
```

## Ubuntu PPA

The Ubuntu packages are also built by Launchpad in the PPA
[`ppa:tumanyan/libriak-c`](https://launchpad.net/~tumanyan/+archive/ubuntu/libriak-c)
for Ubuntu 22.04, 24.04, 26.04 and 26.10, from the release tarball and
`packaging/debian/` (package names as above: `libriak-c2`, `libriak-c-dev`,
`riakctl`):

```sh
sudo add-apt-repository ppa:tumanyan/libriak-c
sudo apt install riakctl
```

Launchpad builds from signed source uploads only. The release workflow
(`.github/workflows/distributions.yml`, job `ppa`) builds the source package
of every series in `PPA_SERIES`, signs it and uploads it after the release is
published; a new Ubuntu release is added to `PPA_SERIES`. For an existing
release the workflow is started by hand:

```sh
gh workflow run distributions.yml -f version=2.2.2
```

The source packages are versioned `<version>-1~<series>1`; Launchpad never
accepts a version twice. `packaging/ppa-upload.sh` skips the versions already
in the PPA and starts a failed FTP session again, so a failed `ppa` job is
completed by running it again (`gh run rerun RUN_ID --failed`). They are
signed with the PPA upload key
`08A2F95F0E0E49BE72DB96A1A0DBBC93C96C4E77`
(`libriak-c PPA upload key <arthurtumanyan@gmail.com>`), which is registered
on the Launchpad account `tumanyan` and serves no other purpose; Launchpad
signs the PPA with its own per-archive key. The key is the repository secret
`PPA_GPG_KEY`, its passphrase `PPA_GPG_PASSPHRASE`. The same steps by hand:

```sh
packaging/ppa-build.sh 2.2.2 08A2F95F0E0E49BE72DB96A1A0DBBC93C96C4E77 ppa-out \
    jammy noble resolute stonking   # dpkg-dev, debhelper, devscripts, curl
packaging/ppa-upload.sh ppa-out
```

## Prebuilt packages in the source tree

Prebuilt x86_64 packages of 2.1.0 and 2.2.0 are also kept in the source tree
under `packages/<version>/<distribution>/`, together with a `SHA256SUMS`
file, for Ubuntu 22.04 and 24.04, Debian 12, Fedora 42 and AlmaLinux 10.
In 2.2.0, `libriak-c-tools` holds `riakctl`. Each set was installed
into a clean container of its distribution and passed
`packaging/smoke-test.sh`.

Installed from these files, without the repository, the development and
tools packages need the runtime package of exactly the same version in the
same command (or installed beforehand), because apt and dnf resolve the
dependencies of a local file only from configured repositories. A tools
package named alone is refused:

```text
libriak-c-tools : Depends: libriak-c2 (= 2.2.0-1~ubuntu24.04) but it is not installable
```

```sh
cd packages/2.2.0 && sha256sum -c SHA256SUMS
sudo apt install ./ubuntu-24.04/libriak-c2_2.2.0-1~ubuntu24.04_amd64.deb \
                 ./ubuntu-24.04/libriak-c-tools_2.2.0-1~ubuntu24.04_amd64.deb
sudo dnf install ./fedora-42/libriak-c-2.2.0-1.fc42.x86_64.rpm \
                 ./fedora-42/libriak-c-tools-2.2.0-1.fc42.x86_64.rpm
```

The development packages (`libriak-c-dev_*.deb`, `libriak-c-devel-*.rpm`)
are added to the same command when needed.

## Releases

A release is made by updating `project(VERSION ...)` in `CMakeLists.txt`,
the version macros in `include/riak/riak.h`, `PROJECT_NUMBER` in
`docs/Doxyfile`, `Version` and the `%changelog` entry in
`packaging/rpm/libriak-c.spec` (Copr builds from it) and the `CHANGELOG.md`
heading, and pushing a matching signed tag:

```sh
git tag -s v2.2.3 -m "libriak-c 2.2.3"
git push origin v2.2.3
```

No secret key material is kept on the development machine. For signing the
tag, the release key is imported from its offline backup
(`gpg --import release-key-secret.asc`) and deleted again afterwards
(`gpg --delete-secret-keys D3C2A1C79D9B63A5DAEF362507E49B41B39FBE84`); the
public key stays in the keyring for verification.

`.github/workflows/release.yml` then checks that the tag matches the project
version, builds and install-tests packages in containers of Ubuntu 22.04,
24.04 and 26.04, Debian 12 and 13, Fedora 44 and 45 and AlmaLinux 9 and 10,
and publishes them with a source tarball, `SHA256SUMS` and build provenance
attestations. The release notes are taken from the `CHANGELOG.md` section of
the version. The distribution releases are listed in the `deb` and `rpm`
matrices of the workflow and are updated when a distribution publishes or
retires a release.

The release workflow then runs `.github/workflows/repository.yml`, which
downloads the packages of all published releases, builds the repositories
with `packaging/repo-site.sh` (sorting by distribution) and
`packaging/make-repo.sh` (indexes and signatures), and deploys them to
GitHub Pages, and `.github/workflows/distributions.yml`, which submits the
release to the Ubuntu PPA and to Fedora Copr (see above). The Pages workflow
can also be started by hand, for example after a release was edited:

```sh
gh workflow run repository.yml
```

It needs GitHub Pages enabled with GitHub Actions as the source, `v*` tags
allowed to deploy (the `github-pages` environment that GitHub creates with
Pages accepts the default branch only, and the release workflow deploys from
the release tag), and the signing key as repository secrets: `RELEASE_GPG_KEY` (the armored secret
key) and, for a key protected by a passphrase, `RELEASE_GPG_PASSPHRASE`.

The release signing key is dedicated to libriak-c. Its primary key
`D3C2A1C79D9B63A5DAEF362507E49B41B39FBE84` is kept offline; CI holds only the signing subkey, exported with
`--export-secret-subkeys` and protected by a passphrase of its own. Metadata
and tags signed by the subkey verify against the public key of the primary.

```sh
gh api -X POST repos/arthurtumanyan/libriak-c/pages -f build_type=workflow
gh api -X POST repos/arthurtumanyan/libriak-c/environments/github-pages/deployment-branch-policies \
    -f name='v*' -f type=tag
gpg --armor --export-secret-subkeys D3C2A1C79D9B63A5DAEF362507E49B41B39FBE84 |
    gh secret set RELEASE_GPG_KEY    # the subkey passphrase goes to RELEASE_GPG_PASSPHRASE
```

The default `GITHUB_TOKEN` permission of the repository is read-only and
workflows may not approve pull requests; every workflow declares the
permissions it needs, and only the workflows started by a release tag or by
hand (release, repository, distributions) receive secrets.

The public key is published as `libriak-c.asc` at the repository root. When
the key is replaced, apt reports `NO_PUBKEY` until the key file is downloaded
again into `/etc/apt/keyrings/libriak-c.asc`; dnf asks to import the new key.
