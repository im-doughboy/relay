#!/usr/bin/env bash
# Package Relay for Windows, from the MSYS2 UCRT64 shell:
#
#     ./packaging/package-windows.sh
#
# Makes, in dist/:
#   Relay/                              relay.exe with everything it needs (runs on its own)
#   Relay-<version>-windows-x64.zip     that folder, zipped
#   Relay-<version>-setup.exe           an installer (when NSIS is installed:
#                                       pacman -S mingw-w64-ucrt-x86_64-nsis)
#
# Needs: pacman -S mingw-w64-ucrt-x86_64-qt6-tools   (for windeployqt)
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

# Use MSYS2's UCRT64 tools (what Relay is built with), whichever MSYS2 window
# this was started from.
if [ ! -d /ucrt64/bin ]; then
    echo "Run this from an MSYS2 window (MSYS2 UCRT64 is best)." >&2
    exit 1
fi
if [ "${MSYSTEM:-}" != "UCRT64" ]; then
    echo "Note: this is the ${MSYSTEM:-MSYS} window; using the UCRT64 tools anyway."
    export PATH="/ucrt64/bin:$PATH"
fi
export MINGW_PREFIX=/ucrt64
export MINGW_PACKAGE_PREFIX=mingw-w64-ucrt-x86_64

# Everything needed, checked up front.
missing_pkgs=()
for tool in cmake:cmake ninja:ninja strip:binutils gcc:gcc; do
    command -v "${tool%%:*}" >/dev/null 2>&1 || missing_pkgs+=("$MINGW_PACKAGE_PREFIX-${tool#*:}")
done
command -v windeployqt6 >/dev/null 2>&1 || command -v windeployqt-qt6 >/dev/null 2>&1 ||
    command -v windeployqt >/dev/null 2>&1 || missing_pkgs+=("$MINGW_PACKAGE_PREFIX-qt6-tools")
if [ ${#missing_pkgs[@]} -gt 0 ]; then
    echo "Missing tools. Install them with:" >&2
    echo "    pacman -S --needed ${missing_pkgs[*]}" >&2
    exit 1
fi
VERSION=$(sed -n 's/^project(Relay VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
BUILD=$ROOT/build-release
DIST=$ROOT/dist
APP=$DIST/Relay

step() { printf '\n== %s\n' "$*"; }

# ---- build ---------------------------------------------------------------------
step "Building Relay $VERSION"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DRELAY_BUILD_TESTS=OFF
cmake --build "$BUILD"

rm -rf "$APP"
mkdir -p "$APP"
cmake --install "$BUILD" --prefix "$APP"
strip "$APP/relay.exe"

# ---- Qt ------------------------------------------------------------------------
step "Adding Qt"
DEPLOY=""
for tool in windeployqt6 windeployqt-qt6 windeployqt; do
    if command -v "$tool" >/dev/null 2>&1; then DEPLOY=$tool; break; fi
done
if [ -z "$DEPLOY" ]; then
    echo "windeployqt wasn't found. Install it with:" >&2
    echo "    pacman -S ${MINGW_PACKAGE_PREFIX}-qt6-tools" >&2
    exit 1
fi
"$DEPLOY" --release --qmldir "$ROOT/qml" --no-translations \
          --no-system-d3d-compiler --no-opengl-sw "$APP/relay.exe"

# Tell Qt where its plugins and QML modules are inside this folder (MSYS2's
# Qt would otherwise look for them next to where it was installed).
PLUGINS=.
[ -d "$APP/plugins/platforms" ] && PLUGINS=plugins
QMLDIR=qml
cat > "$APP/qt.conf" <<EOF
[Paths]
Prefix = .
Plugins = $PLUGINS
QmlImports = $QMLDIR
Qml2Imports = $QMLDIR
EOF

# ---- mpv and every other library from MSYS2 ------------------------------------
# windeployqt only knows about Qt. Follow the imports of every program and
# library in the folder and copy whatever comes from MSYS2, until nothing new
# turns up.
step "Adding mpv and the other libraries"
declare -A seen=()
while :; do
    added=0
    while IFS= read -r -d '' file; do
        [ -n "${seen[$file]:-}" ] && continue
        seen[$file]=1
        while IFS= read -r dep; do
            name=$(basename "$dep")
            if [ ! -e "$APP/$name" ]; then
                cp "$dep" "$APP/"
                added=$((added + 1))
            fi
        done < <(ldd "$file" 2>/dev/null | awk -v p="$MINGW_PREFIX/bin/" 'index($3, p) == 1 { print $3 }')
    done < <(find "$APP" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
    [ "$added" -eq 0 ] && break
    echo "   added $added"
done

# ---- licenses ------------------------------------------------------------------
# Relay's own (GPL v3), and the license files of every MSYS2 package a bundled
# library comes from (Qt, mpv, FFmpeg and what they use).
step "Licenses"
prefix=${MINGW_PACKAGE_PREFIX:-mingw-w64-ucrt-x86_64}
cp "$ROOT/LICENSE" "$APP/LICENSE.txt"
mkdir -p "$APP/licenses"
pkgs=$(find "$APP" -type f -iname '*.dll' -print0 \
       | while IFS= read -r -d '' f; do
             src="$MINGW_PREFIX/bin/$(basename "$f")"
             if [ -e "$src" ]; then pacman -Qqo "$src" 2>/dev/null || true; fi
         done | sort -u)
count=0
for pkg in $pkgs; do
    dir="$MINGW_PREFIX/share/licenses/${pkg#"$prefix-"}"
    [ -d "$dir" ] || continue
    cp -r "$dir" "$APP/licenses/"
    count=$((count + 1))
done
{
    echo "Relay is free software under the GNU General Public License v3 (LICENSE.txt)."
    echo "Source: https://github.com/im-doughboy/relay"
    echo
    echo "This folder also contains these open-source packages from MSYS2"
    echo "(https://packages.msys2.org), each under its own license; their license"
    echo "files are in the licenses folder, and their source is at"
    echo "https://github.com/msys2/MINGW-packages and the projects' own sites:"
    echo
    for pkg in $pkgs; do echo "  ${pkg#"$prefix-"} $(pacman -Q "$pkg" | cut -d' ' -f2)"; done
} > "$APP/THIRD-PARTY.txt"
echo "   $count packages' license files"

# ---- check ---------------------------------------------------------------------
step "Checking"
problems=0
need() { if [ ! -e "$APP/$1" ]; then echo "   missing: $1"; problems=1; fi; }
need relay.exe
need libmpv-2.dll
need "$PLUGINS/platforms/qwindows.dll"
need "$PLUGINS/sqldrivers/qsqlite.dll"
need "$QMLDIR/QtQuick/Controls"
ls "$APP/$PLUGINS/tls/"*.dll >/dev/null 2>&1 || { echo "   missing: TLS plugin (https)"; problems=1; }
# With MSYS2 out of the search path, every library must resolve inside the folder.
missing=$(PATH="/usr/bin:/c/Windows/System32" ldd "$APP/relay.exe" 2>&1 | grep -Ei 'not found|\?\?\?' || true)
if [ -n "$missing" ]; then echo "$missing"; problems=1; fi
if [ "$problems" -ne 0 ]; then
    echo "Something is missing (above): the folder may not run on its own." >&2
    exit 1
fi
SIZE=$(du -sm "$APP" | cut -f1)
echo "   ok: $(find "$APP" -type f | wc -l) files, $SIZE MB"

# ---- zip -----------------------------------------------------------------------
step "Zipping"
ZIP=$DIST/Relay-$VERSION-windows-x64.zip
rm -f "$ZIP"
if command -v zip >/dev/null 2>&1; then
    (cd "$DIST" && zip -qr "$(basename "$ZIP")" Relay)
else
    powershell.exe -NoProfile -Command \
        "Compress-Archive -Path '$(cygpath -w "$APP")' -DestinationPath '$(cygpath -w "$ZIP")' -Force"
fi
echo "   $ZIP"

# ---- installer -----------------------------------------------------------------
if command -v makensis >/dev/null 2>&1; then
    step "Making the installer"
    # The uninstaller removes exactly the files installed (never your data);
    # list them, deepest folders last.
    LIST=$BUILD/nsis-uninstall.nsh
    {
        (cd "$APP" && find . -type f | sed 's|^\./||' | sort) | while IFS= read -r f; do
            printf 'Delete "$INSTDIR\\%s"\n' "${f//\//\\}"
        done
        (cd "$APP" && find . -mindepth 1 -type d | sed 's|^\./||' | awk '{ print length, $0 }' | sort -rn | cut -d' ' -f2-) |
            while IFS= read -r d; do
                printf 'RMDir "$INSTDIR\\%s"\n' "${d//\//\\}"
            done
    } > "$LIST"
    makensis -V2 \
        -DVERSION="$VERSION" \
        -DSRC_DIR="$(cygpath -w "$APP")" \
        -DUNINSTALL_LIST="$(cygpath -w "$LIST")" \
        -DICON="$(cygpath -w "$ROOT/packaging/icons/relay.ico")" \
        -DOUT_FILE="$(cygpath -w "$DIST/Relay-$VERSION-setup.exe")" \
        "$(cygpath -w "$ROOT/packaging/relay.nsi")"
    echo "   $DIST/Relay-$VERSION-setup.exe"
else
    step "Installer skipped"
    echo "   For a setup.exe too: pacman -S ${MINGW_PACKAGE_PREFIX}-nsis, then run this again."
fi

step "Done"
echo "   Try dist/Relay/relay.exe by double-clicking it in Explorer (not from this shell),"
echo "   so it's tested without MSYS2's libraries."
