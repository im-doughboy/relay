#!/usr/bin/env bash
# Fixes pacman's "duplicated database entry" errors (an MSYS2 update that was
# interrupted can leave two versions of a package in pacman's list of installed
# packages). Keeps the newest version of each and moves the others to a backup
# folder; nothing is deleted. Run from any MSYS2 window, then: pacman -Syu
set -euo pipefail

DB=${PACMAN_DB:-/var/lib/pacman/local}
BACKUP="$DB-duplicates-$(date +%Y%m%d-%H%M%S)"
cd "$DB"

base() { sed -E 's/-[^-]+-[^-]+$//' <<<"$1"; }   # name-1.2.3-4 -> name

entries=()
for d in */; do entries+=("${d%/}"); done

moved=0
while IFS= read -r name; do
    [ -n "$name" ] || continue
    versions=()
    for d in "${entries[@]}"; do
        [ "$(base "$d")" = "$name" ] && versions+=("$d")
    done
    newest=${versions[0]}
    for d in "${versions[@]:1}"; do
        if [ "$(vercmp "${d#"$name"-}" "${newest#"$name"-}")" -gt 0 ]; then newest=$d; fi
    done
    for d in "${versions[@]}"; do
        [ "$d" = "$newest" ] && continue
        mkdir -p "$BACKUP"
        mv "$d" "$BACKUP/"
        echo "kept $newest, moved aside $d"
        moved=$((moved + 1))
    done
done < <(for d in "${entries[@]}"; do base "$d"; done | sort | uniq -d)

if [ "$moved" -eq 0 ]; then
    echo "No duplicates found."
else
    echo "Moved $moved old entries to $BACKUP"
    echo "Now run: pacman -Syu"
fi
