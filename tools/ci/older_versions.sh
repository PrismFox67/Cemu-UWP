#!/usr/bin/env bash
# Keeps every Xbox build on the older-versions pre-release (ci-latest is replaced on every build and CI artifacts expire
# after 90 days) and rewrites its notes as a table, newest first.
# Usage: older_versions.sh <cemu-uwp-x64.zip> <version>   adds one build, then rewrites the notes
#        older_versions.sh                                only rewrites the notes
set -euo pipefail
repo=${GITHUB_REPOSITORY:-PrismFox67/Cemu-UWP}
tag=older-versions
base="https://github.com/$repo"

if [ $# -ge 2 ]; then
  name="cemu-uwp-x64-$2.zip"
  cp "$1" "$name"
  label="$2 ($(date -u +%m/%d/%Y), ${GITHUB_SHA::7})"
  gh release view "$tag" -R "$repo" >/dev/null 2>&1 ||
    gh release create "$tag" -R "$repo" --prerelease --latest=false --title "Older versions" --notes "Older Xbox builds"
  for i in 1 2 3 4; do gh release upload "$tag" "$name#$label" -R "$repo" --clobber && break; sleep 20; done
fi

notes=$(mktemp)
cat > "$notes" <<EOF
Every Xbox app build that has been on [ci-latest]($base/releases/tag/ci-latest), newest first. Useful when something used to work and you want to find the build where it changed. For normal play, use ci-latest.

- **No GitHub account needed**, just click a file. New builds are added here automatically.
- **Installing:** same as any other build, see the [Xbox tester guide]($base/blob/d3d12-backend/docs/XBOX_TESTERS.md).
- **Going back to an older build:** it has a lower version number than the one installed, so it may refuse to install over it. If it does, uninstall Cemu first. Uninstalling can remove what's in Cemu's own storage, so back up your saves first (Settings → Logs and backups), or keep them on USB (Settings → Folders → Save folder).
- **What changed when:** [CHANGELOG.md]($base/blob/d3d12-backend/CHANGELOG.md) lists changes by date.

| Build | Date | Download | Commit |
|---|---|---|---|
EOF
gh api "repos/$repo/releases/tags/$tag" --jq '.assets[] | select(.name | startswith("cemu-uwp-x64-")) | [.label, .name, .browser_download_url] | @tsv' |
  sed -E 's/^([0-9]+\.[0-9]+\.([0-9]+)) \(([0-9\/]+), ([0-9a-f]+)\)\t([^\t]+)\t(.*)$/\2\t\1\t\3\t\4\t\5\t\6/' |
  sort -t$'\t' -k1,1nr |
  awk -F'\t' -v base="$base" '{ printf "| %s | %s | [%s](%s) | [%s](%s/commit/%s) |\n", $2, $3, $5, $6, $4, base, $4 }' >> "$notes"
gh release edit "$tag" -R "$repo" --notes-file "$notes"
rm -f "$notes"
