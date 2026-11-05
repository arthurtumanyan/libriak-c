# Source package for Fedora Copr (and any rpmbuild): built from the release
# tarball into the same three packages as cpack -G RPM.

Name:           libriak-c
Version:        2.2.3
Release:        1%{?dist}
Summary:        C client for Riak KV (Protocol Buffers and HTTP)
License:        GPL-3.0-or-later AND Apache-2.0
URL:            https://github.com/arthurtumanyan/libriak-c
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  cmake >= 3.20
BuildRequires:  make
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(json-c)
BuildRequires:  pkgconfig(libprotobuf-c)
# protoc-c: protobuf-c-compiler, or protobuf3-c-compiler from Fedora 45 on
BuildRequires:  /usr/bin/protoc-c
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(zlib)

%description
C client library for Riak KV over Protocol Buffers and HTTP, with TLS,
authentication, connection pooling and an asynchronous API. The generated
Protocol Buffers code comes from riak_pb (Apache-2.0).

%package        devel
Summary:        Development files for %{name}
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       pkgconfig(libcurl)
Requires:       pkgconfig(json-c)
Requires:       pkgconfig(libprotobuf-c)
Requires:       pkgconfig(openssl)
Requires:       pkgconfig(zlib)

%description    devel
Headers, static library, CMake package and pkg-config file for %{name}.

%package     -n riakctl
Summary:        Command-line client for Riak KV
Requires:       %{name}%{?_isa} = %{version}-%{release}
# shipped as libriak-c-tools up to 2.2.1
Provides:       libriak-c-tools = %{version}-%{release}
Obsoletes:      libriak-c-tools < 2.2.2

%description -n riakctl
riakctl, a scriptable command-line client for Riak KV built on %{name}:
objects, backup and restore, queries, data types, bucket properties,
Search, AAE and replication, with JSON output.

%prep
%autosetup -p1

%build
%cmake -DRIAK_BUILD_TESTS=OFF -DRIAK_BUILD_EXAMPLES=OFF -DRIAK_BUILD_TOOLS=ON
%cmake_build

%install
%cmake_install
# README and CHANGELOG are listed through %%doc
rm -rf %{buildroot}%{_docdir}/%{name}

%files
%license proto/LICENSE
%doc README.md CHANGELOG.md
%{_libdir}/libriak-c.so.2
%{_libdir}/libriak-c.so.%{version}

%files devel
%{_includedir}/riak/
%{_libdir}/libriak-c.so
%{_libdir}/libriak-c.a
%{_libdir}/cmake/libriak-c/
%{_libdir}/pkgconfig/libriak-c.pc

%files -n riakctl
%{_bindir}/riakctl
%{_mandir}/man1/riakctl.1*

%changelog
* Thu Nov 05 2026 Arthur Tumanyan <arthurtumanyan@gmail.com> - 2.2.3-1
- Release 2.2.3

* Wed Nov 04 2026 Arthur Tumanyan <arthurtumanyan@gmail.com> - 2.2.2-1
- Release 2.2.2; riakctl is packaged as riakctl in place of libriak-c-tools

* Tue Nov 03 2026 Arthur Tumanyan <arthurtumanyan@gmail.com> - 2.2.1-1
- Release 2.2.1
