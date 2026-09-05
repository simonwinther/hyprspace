#!/usr/bin/env bash
# Archive only the clean, committed version that has passed release checks.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

tag=${1:?Usage: bash scripts/dist.sh vX.Y.Z}
python3 scripts/check-release.py --tag "$tag"
if [[ -n $(git status --porcelain --untracked-files=normal) ]]; then
    echo 'Commit the release files before making an archive.' >&2
    exit 1
fi
tag_commit=$(git rev-parse --verify "refs/tags/$tag^{commit}")
if [[ $tag_commit != "$(git rev-parse HEAD)" ]]; then
    echo 'Check out the version tag before making its archive.' >&2
    exit 1
fi

mkdir -p dist
archive="hyprspace-${tag#v}.tar.gz"
staging=$(mktemp -d dist/.archive.XXXXXX)
trap 'rm -rf -- "$staging"' EXIT
git archive --format=tar --prefix="hyprspace-${tag#v}/" "$tag_commit" | gzip -n > "$staging/$archive"
(cd "$staging" && sha256sum "$archive" > SHA256SUMS)
mv -- "$staging/$archive" "$staging/SHA256SUMS" dist/
echo "Created dist/$archive and dist/SHA256SUMS"
