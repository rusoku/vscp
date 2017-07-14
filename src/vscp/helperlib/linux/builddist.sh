#!/bin/bash

# get version from the version file
MAJOR_VERSION=`sed '35!d' ../../common/version.h  | cut -b 33-`
MINOR_VERSION=`sed '36!d' ../../common/version.h  | cut -b 33-`
RELEASE_VERSION=`sed '37!d' ../../common/version.h  | cut -b 33-`
LIB_PLUS_VER="libvscphelper.so.$MAJOR_VERSION.$MINOR_VERSION.$RELEASE_VERSION"

#Build debian
checkinstall --type="debian" --pkgname="libvscphelper-dev" --pkgversion="$MAJOR_VERSION.$MINOR_VERSION.$RELEASE_VERSION" --pkgrelease="1" --pkglicense="MIT" --pkggroup="developer" --pkgsource="https://github.com/grodansparadis/vscp" --maintainer="akhe@grodansparadis.com" --requires="libwxbase3.0-dev \(\>=3.0.0\) ,libssl-dev"

#build rpm
checkinstall --type="rpm" --pkgname="libvscphelper-dev" --pkgversion="$MAJOR_VERSION.$MINOR_VERSION.$RELEASE_VERSION" --pkgrelease="1" --pkglicense="MIT" --pkggroup="developer" --pkgsource="https://github.com/grodansparadis/vscp" --maintainer="akhe@grodansparadis.com" --requires="libwxbase3.0-dev \(\>=3.0.0\) ,libssl-dev"
