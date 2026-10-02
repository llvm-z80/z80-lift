#!/usr/bin/env bash
# Links every llvm-z80 runtime function into one ELF image per target, for
# z80-lift to read. Build the archives first with `ninja Z80Runtime`.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 <llvm-z80-build-dir> [<out-dir>]" >&2
  exit 2
fi

build=$1
out=${2:-$(cd "$(dirname "$0")/.." && pwd)/build/images}
lld=$build/bin/ld.lld

if [[ ! -x $lld ]]; then
  echo "error: $lld not found; build lld in $build" >&2
  exit 1
fi

mkdir -p "$out"
for target in z80 sm83; do
  lib=$build/lib/$target
  for f in "$lib/${target}_rt.a" "$lib/$target.ld"; do
    if [[ ! -e $f ]]; then
      echo "error: $f not found; run 'ninja Z80Runtime' in $build" >&2
      exit 1
    fi
  done

  # No crt0, so there is no _start; the entry address is irrelevant here.
  "$lld" -T "$lib/$target.ld" -e 0 \
    --whole-archive "$lib/${target}_rt.a" --no-whole-archive \
    -o "$out/$target-runtime.elf"
  echo "$out/$target-runtime.elf"
done
