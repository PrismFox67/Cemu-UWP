#!/bin/bash
# Writes the ci-latest release notes so a visitor sees what changed since the last time they looked: the newest
# sections of CHANGELOG.md and every commit since the previous build (the previous build's commit is kept in the notes
# as an HTML comment). Run by CI after the uploads; needs GH_TOKEN (contents: write), GITHUB_REPOSITORY, GITHUB_SHA and
# GITHUB_RUN_NUMBER (the Xbox package version is 0.1.<run number>.0). The "Downloads so far" line that
# keep_download_count.sh --json maintains is kept.
set -u
REL=ci-latest
R=${GITHUB_REPOSITORY:-$(gh repo view --json nameWithOwner --jq .nameWithOwner)}
SHA=${GITHUB_SHA:-$(git rev-parse HEAD)}
SHORT=${SHA::7}
PKG="0.1.${GITHUB_RUN_NUMBER:-0}.0"
BLOB="https://github.com/$R/blob/d3d12-backend"

body=$(gh release view $REL --json body --jq .body 2>/dev/null || true)
prev=$(sed -n 's/.*<!-- build-sha: \([0-9a-f]*\) -->.*/\1/p' <<<"$body" | head -n 1)
downloads=$(grep '^Downloads so far' <<<"$body" | tail -n 1)

# the newest two dated sections of CHANGELOG.md, headings one level down so they nest under "What's new"
whatsnew=$(awk '/^## /{n++} n>=1 && n<=2' CHANGELOG.md | sed 's/^###/####/; s/^## /### /')

commits=""
if [ -n "$prev" ] && [ "$prev" != "$SHA" ]; then
  commits=$(gh api "repos/$R/compare/$prev...$SHA" --paginate \
    --jq '.commits[] | "- " + (.commit.message | split("\n")[0]) + " (" + .sha[0:7] + ")"' 2>/dev/null || true)
fi
if [ -n "$commits" ]; then
  since="## Commits since the previous build (${prev::7} → $SHORT)
$commits"
elif [ -n "$prev" ] && [ "$prev" = "$SHA" ]; then
  since="## Commits since the previous build
This build is a rerun of the same commit."
else
  since="## Commits since the previous build
See the [commit history](https://github.com/$R/commits/d3d12-backend)."
fi

notes="Automatic Windows and Xbox (UWP) builds of this fork, refreshed on every push to \`d3d12-backend\`. Not an official Cemu release.

**Build:** \`$SHORT\` (Xbox package $PKG) · [Xbox setup and recommended settings]($BLOB/docs/XBOX_TESTERS.md) · [Compatibility list]($BLOB/docs/COMPATIBILITY.md) · [Full changelog]($BLOB/CHANGELOG.md)

| File | What it is |
|---|---|
| \`cemu-uwp-x64.zip\` | **The Xbox app.** Install it through the Xbox Device Portal (as an update over the previous build). |
| \`cemu-d3d12-windows-x64.zip\` | Regular Cemu for Windows with the Direct3D 12 renderer added. |
| \`cemu-host-win32-x64.zip\` | The Xbox frontend as a Windows program, for testing it on a PC (\`-pdb\` has the debug symbols). |
| \`MK8_FirstPerson.zip\` | The Mario Kart 8 first person pack on its own, for other Cemu builds (the builds above include it). |

## What's new
$whatsnew

$since

<!-- build-sha: $SHA -->
${downloads:+
$downloads}"

gh release edit $REL --notes "$notes" && echo "release notes updated for $SHORT (previous build ${prev:-none})"
