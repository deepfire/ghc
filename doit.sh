#!/bin/sh
set -e

XFLAGS=-XStructuredImports
# DFLAGS=
DFLAGS="-ddump-rn-trace" # -ddump-hi-diffs -ddump-if-trace"
FLAVOR=quickest

#################
rm -f *.o *.hi *~
./hadrian/build.cabal.sh --freeze1 --flavour=${FLAVOR} -j stage2:exe:ghc-bin
_build/stage1/bin/ghc M3.hs -XNoImplicitPrelude -XStructuredImports ${DFLAGS}
# _build/stage1/bin/ghc --show-iface Begin.hi
