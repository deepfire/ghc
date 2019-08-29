#!/bin/sh
set -e

XFLAGS=-XStructuredImports
DFLAGS=
# DFLAGS="-ddump-rn-trace" # -ddump-hi-diffs -ddump-if-trace"
FLAVOR=quickest
KEEP=
while test -n "$1"
do case $1 in
           --keep ) KEEP=t;;
           --* ) echo 'ERROR: unknown arg: '$1 >&2; exit 1;;
           * ) break;;
   esac; shift; done

#################
rm -f *.o *.hi *~
test -n "${KEEP}" ||
./hadrian/build.cabal.sh --freeze1 --flavour=${FLAVOR} -j stage2:exe:ghc-bin
_build/stage1/bin/ghc M3.hs -XNoImplicitPrelude -XStructuredImports ${DFLAGS}
# _build/stage1/bin/ghc --show-iface Begin.hi
