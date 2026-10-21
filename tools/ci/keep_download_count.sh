#!/bin/bash
# Keeps a running download total for a ci-latest asset. GitHub resets an asset's download_count when it is replaced
# (gh release upload --clobber), so call this right before replacing it: it adds the old asset's count to a total kept
# in the "download-stats" release as <asset>.count.json = {"base": downloads of earlier builds, "counted_id": id of the
# asset already added, "counted": its count at that time}. A rerun before the replacement happened (same asset id)
# re-counts instead of adding twice.
#   usage: tools/ci/keep_download_count.sh <asset name>        (needs GH_TOKEN with contents: write)
#          tools/ci/keep_download_count.sh --show               (prints the running totals)
set -u
STATS=download-stats
REL=ci-latest

gh release view $STATS >/dev/null 2>&1 || gh release create $STATS --prerelease --title "Download stats" \
  --notes "Running download totals of the ci-latest builds (GitHub resets a file's count when CI replaces it). Not a build." >/dev/null 2>&1 || true

field() { sed -n "s/.*\"$2\": *\([0-9]*\).*/\1/p" <<<"$1"; }

read_stats() {   # asset -> json text (or defaults)
  local tmp; tmp=$(mktemp -d)
  if gh release download $STATS -p "$1.count.json" -D "$tmp" >/dev/null 2>&1; then cat "$tmp/$1.count.json"; else echo '{"base": 0, "counted_id": 0, "counted": 0}'; fi
  rm -rf "$tmp"
}

current() {      # asset -> "id count" of the asset now on ci-latest (empty if none)
  gh api "repos/$GITHUB_REPOSITORY/releases/tags/$REL" --jq ".assets[] | select(.name == \"$1\") | \"\(.id) \(.download_count)\"" 2>/dev/null
}

if [ "${1:-}" = "--show" ]; then
  GITHUB_REPOSITORY=${GITHUB_REPOSITORY:-$(gh repo view --json nameWithOwner --jq .nameWithOwner)}
  for a in $(gh api "repos/$GITHUB_REPOSITORY/releases/tags/$REL" --jq '.assets[].name'); do
    s=$(read_stats "$a"); base=$(field "$s" base); cid=$(field "$s" counted_id); cnt=$(field "$s" counted)
    read -r id n <<<"$(current "$a")"
    [ "${id:-0}" = "$cid" ] && n=$(( ${n:-0} - cnt ))
    echo "$a: $(( base + ${n:-0} )) total"
  done
  exit 0
fi

A=$1
read -r id n <<<"$(current "$A")"
[ -z "${id:-}" ] && exit 0   # nothing to count yet
s=$(read_stats "$A"); base=$(field "$s" base); cid=$(field "$s" counted_id); cnt=$(field "$s" counted)
if [ "$id" = "$cid" ]; then base=$(( base - cnt + n )); else base=$(( base + n )); fi
tmp=$(mktemp -d)
printf '{"base": %d, "counted_id": %d, "counted": %d}\n' "$base" "$id" "$n" > "$tmp/$A.count.json"
for i in 1 2 3; do gh release upload $STATS "$tmp/$A.count.json" --clobber >/dev/null 2>&1 && break; sleep 10; done
rm -rf "$tmp"
echo "download total for $A: $base (before this build)"
