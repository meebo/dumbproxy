#!/bin/sh
mkdir -p build
rpmbuild --define="_builddir build" -v -bb dumbproxy.spec
rm -Rf build
