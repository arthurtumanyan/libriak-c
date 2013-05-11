#!/bin/bash -x

#
# Generated - do not edit!
#

# Macros
TOP=`pwd`
CND_PLATFORM=GNU-Linux-x86
CND_CONF=Release
CND_DISTDIR=dist
CND_BUILDDIR=build
NBTMPDIR=${CND_BUILDDIR}/${CND_CONF}/${CND_PLATFORM}/tmp-packaging
TMPDIRNAME=tmp-packaging
OUTPUT_PATH=${CND_DISTDIR}/${CND_CONF}/${CND_PLATFORM}/libriak-c.${CND_DLIB_EXT}
OUTPUT_BASENAME=libriak-c.${CND_DLIB_EXT}
PACKAGE_TOP_DIR=/usr/

# Functions
function checkReturnCode
{
    rc=$?
    if [ $rc != 0 ]
    then
        exit $rc
    fi
}
function makeDirectory
# $1 directory path
# $2 permission (optional)
{
    mkdir -p "$1"
    checkReturnCode
    if [ "$2" != "" ]
    then
      chmod $2 "$1"
      checkReturnCode
    fi
}
function copyFileToTmpDir
# $1 from-file path
# $2 to-file path
# $3 permission
{
    cp "$1" "$2"
    checkReturnCode
    if [ "$3" != "" ]
    then
        chmod $3 "$2"
        checkReturnCode
    fi
}

# Setup
cd "${TOP}"
mkdir -p ${CND_DISTDIR}/${CND_CONF}/${CND_PLATFORM}/package
rm -rf ${NBTMPDIR}
mkdir -p ${NBTMPDIR}

# Copy files and create directories and links
cd "${TOP}"
makeDirectory "${NBTMPDIR}//usr"
copyFileToTmpDir "callbacks.c" "${NBTMPDIR}/${PACKAGE_TOP_DIR}callbacks.c" 0644

cd "${TOP}"
makeDirectory "${NBTMPDIR}//usr"
copyFileToTmpDir "riak-c.c" "${NBTMPDIR}/${PACKAGE_TOP_DIR}riak-c.c" 0644

cd "${TOP}"
makeDirectory "${NBTMPDIR}//usr"
copyFileToTmpDir "riak-c.h" "${NBTMPDIR}/${PACKAGE_TOP_DIR}riak-c.h" 0644

cd "${TOP}"
makeDirectory "${NBTMPDIR}//usr"
copyFileToTmpDir "dist/Release/GNU-Linux-x86/libriak-c.so" "${NBTMPDIR}/${PACKAGE_TOP_DIR}libriak-c.so" 0755


# Create control file
cd "${TOP}"
CONTROL_FILE=${NBTMPDIR}/DEBIAN/control
rm -f ${CONTROL_FILE}
mkdir -p ${NBTMPDIR}/DEBIAN

cd "${TOP}"
echo 'Package: liblibriak-c.so' >> ${CONTROL_FILE}
echo 'Version: 1.0' >> ${CONTROL_FILE}
echo 'Architecture: amd64' >> ${CONTROL_FILE}
echo 'Maintainer: Arthur Tumanyan <arthurtumanyan@yahoo.com; www.arthurtumanyan.tk>' >> ${CONTROL_FILE}
echo 'Description: An curl based RIAK client library written in ANSI C ' >> ${CONTROL_FILE}
echo 'Depends: curl library' >> ${CONTROL_FILE}
echo 'Depends: json-c library' >> ${CONTROL_FILE}

# Create Debian Package
cd "${TOP}"
cd "${NBTMPDIR}/.."
dpkg-deb  --build ${TMPDIRNAME}
checkReturnCode
cd "${TOP}"
mkdir -p  ${CND_DISTDIR}/${CND_CONF}/${CND_PLATFORM}/package
mv ${NBTMPDIR}.deb ${CND_DISTDIR}/${CND_CONF}/${CND_PLATFORM}/package/libriak-c.so.deb
checkReturnCode
echo Debian: ${CND_DISTDIR}/${CND_CONF}/${CND_PLATFORM}/package/libriak-c.so.deb

# Cleanup
cd "${TOP}"
rm -rf ${NBTMPDIR}
