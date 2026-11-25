#!/bin/sh
# Copies the built mod into the game folder. Run from the repo root.
# Layout (libfpv INTEGRATION.md "Label the mod libfpv inside the game"):
#   GTAIV/libfpv.asi                 the loader (the ASI loader picks it up)
#   GTAIV/libfpv/libfpv_gtaiv.dll    the mod itself, hot-reloaded
#   GTAIV/libfpv/libfpv.dll          libfpv
# libfpv comes from build/win32 of the libfpv checkout (LIBFPV_DIR, default
# ~/Projects/libfpv), or build/win32-kamikaze on a kamikaze dev machine
# (LIBFPV_KAMIKAZE_DEV=1 or true); LIBFPV_DLL picks another one. The mod must
# be built for the same kind (GTAIV_FPV_KAMIKAZE): fpv_frame differs.
set -e
GAME="$(cd "$(dirname "$0")/../GTAIV" && pwd)"
DEST="$GAME/libfpv"
LIBFPV="${LIBFPV_DIR:-$HOME/Projects/libfpv}"
case "$LIBFPV_KAMIKAZE_DEV" in 1|true) KAMIKAZE=ON; LIB_DEFAULT="$LIBFPV/build/win32-kamikaze/libfpv.dll" ;;
  *) KAMIKAZE=OFF; LIB_DEFAULT="$LIBFPV/build/win32/libfpv.dll" ;; esac
LIB="${LIBFPV_DLL:-$LIB_DEFAULT}"
[ -f "$LIB" ] || { echo "missing $LIB: build libfpv first" >&2; exit 1; }
MOD_KAMIKAZE=$(sed -n 's/^GTAIV_FPV_KAMIKAZE:BOOL=//p' build/CMakeCache.txt)
LIB_KAMIKAZE=OFF
grep -q fpv_kamikaze_poll_detonation "$LIB" && LIB_KAMIKAZE=ON
[ "$MOD_KAMIKAZE" = "$LIB_KAMIKAZE" ] || {
  echo "the mod is built with GTAIV_FPV_KAMIKAZE=$MOD_KAMIKAZE but $LIB is kamikaze=$LIB_KAMIKAZE:" >&2
  echo "  cmake -S . -B build -DGTAIV_FPV_KAMIKAZE=$LIB_KAMIKAZE && cmake --build build" >&2
  exit 1; }
[ "$LIB_KAMIKAZE" = "$KAMIKAZE" ] || [ -n "$LIBFPV_DLL" ] || echo "note: LIBFPV_KAMIKAZE_DEV says kamikaze=$KAMIKAZE"
mkdir -p "$DEST"
# Versions before the libfpv naming: the new loader moves these into
# libfpv/old at the next start if they are still there (locked while the game runs).
for f in GTAIV.FPV.asi GTAIV.FPV.core.dll GTAIV.FPV.core.live.dll libfpv.dll libfpv.LICENSE.txt; do
  [ -e "$GAME/$f" ] && { rm -f "$GAME/$f" 2>/dev/null && echo "removed old $f" || echo "old $f in use, the loader moves it at the next start"; }
done
# The loader is locked while the game runs; it only changes rarely.
cp build/libfpv.asi "$GAME/" 2>/dev/null || echo "loader in use, kept the running one"
# The running game hot-reloads the core within a second of this copy.
cp build/libfpv_gtaiv.dll "$DEST/"
cp "$LIB" "$DEST/" 2>/dev/null || echo "libfpv.dll in use, kept the running one (restart the game for a new libfpv)"
cp "$LIBFPV/LICENSE" "$DEST/libfpv.LICENSE.txt"
echo "deployed to $GAME (libfpv kamikaze=$LIB_KAMIKAZE from $LIB)"
