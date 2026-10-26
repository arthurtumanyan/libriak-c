# Binary packages: cpack -G DEB / cpack -G RPM / cpack -G TGZ
#
# Two components per format:
#   runtime  libriak-c2 (deb) / libriak-c (rpm)        shared library, docs
#   dev      libriak-c-dev (deb) / libriak-c-devel (rpm) headers, static lib,
#            .so link, CMake package and pkg-config file

set(CPACK_PACKAGE_NAME libriak-c)
set(CPACK_PACKAGE_VENDOR "Arthur Tumanyan")
set(CPACK_PACKAGE_CONTACT "Arthur Tumanyan <arthurtumanyan@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/arthurtumanyan/libriak-c")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_README ${PROJECT_SOURCE_DIR}/README.md)
set(CPACK_PACKAGE_FILE_NAME ${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR})
set(CPACK_STRIP_FILES ON)
set(CPACK_COMPONENTS_ALL runtime dev)
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Riak KV HTTP client library - shared library")
set(CPACK_COMPONENT_DEV_DESCRIPTION "Riak KV HTTP client library - development files")
set(CPACK_COMPONENT_DEV_DEPENDS runtime)
set(CPACK_SOURCE_GENERATOR TGZ)
set(CPACK_SOURCE_IGNORE_FILES /\\.git/ /build.*/ /graphify-out/)

set(RIAK_PKG_RELEASE 1 CACHE STRING "Package release number")
set(RIAK_DEB_DIST_SUFFIX "" CACHE STRING "Debian release suffix, e.g. ~ubuntu24.04")

# Debian
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_PACKAGE_RELEASE "${RIAK_PKG_RELEASE}${RIAK_DEB_DIST_SUFFIX}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION libs)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME libriak-c${PROJECT_VERSION_MAJOR})
set(CPACK_DEBIAN_DEV_PACKAGE_NAME libriak-c-dev)
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION libdevel)
set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS
  "libriak-c${PROJECT_VERSION_MAJOR} (= ${PROJECT_VERSION}-${CPACK_DEBIAN_PACKAGE_RELEASE}), libcurl4-openssl-dev | libcurl-dev, libjson-c-dev")

# RPM
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE ${RIAK_PKG_RELEASE})
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)  # appends %{dist}, e.g. 1.fc44
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "System Environment/Libraries")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_RUNTIME_PACKAGE_NAME libriak-c)
set(CPACK_RPM_DEV_PACKAGE_NAME libriak-c-devel)
set(CPACK_RPM_DEV_PACKAGE_REQUIRES "libriak-c = ${PROJECT_VERSION}, libcurl-devel, json-c-devel")
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)

include(CPack)
