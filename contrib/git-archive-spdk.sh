#!/bin/bash
set -e
WD=$(dirname $(dirname $(readlink -f $0)))
cd $WD
if [ -e /etc/redhat-release  ] ; then
    ODIR=~/rpmbuild/SOURCES
else
    ODIR=..
fi
# VER=18.10
# VER=19.03
VER=20.01
git submodule init
git submodule update
for mod in dpdk ocf intel-ipsec-mb isa-l ; do
    (cd $mod;
      git archive \
            --format=tar.gz --prefix=$mod/ -o $ODIR/spdk-$mod-$VER.tar.gz  HEAD
    )
done

git archive --format=tar.gz --prefix=spdk-$VER/ -o $ODIR/spdk-$VER.tar.gz  HEAD
