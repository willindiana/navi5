#!/usr/bin/env bash
# Navi5 — make sure the CI job image for the current inputs exists, and name it.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The tag is a hash of every file in ci/ plus the toolchain inputs listed in
# ci/toolchain-inputs (scripts/ci-inputs.sh), so unchanged inputs reuse the
# published image at once; a change builds and pushes a new tag. The build
# context is a staging copy of exactly those files, so nothing unhashed can
# reach the image. The registry is the Forgejo instance running the job (taken
# from GITHUB_SERVER_URL, never written into the repository). Needs DOCKER_HOST
# (the runner's Docker daemon) and CI_TOKEN, the owner's shared package token
# (PACKAGES_TOKEN secret).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

tag="$(bash scripts/ci-inputs.sh --hash | cut -c1-16)"
if [[ "${1:-}" == "--tag" ]]; then echo "$tag"; exit 0; fi

: "${GITHUB_SERVER_URL:?}" "${GITHUB_REPOSITORY_OWNER:?}" "${CI_TOKEN:?}"
registry="${GITHUB_SERVER_URL#*://}"; registry="${registry%%/*}"
owner="$(tr '[:upper:]' '[:lower:]' <<<"$GITHUB_REPOSITORY_OWNER")"
image="${registry}/${owner}/navi5-ci:${tag}"

docker login "$registry" -u "$GITHUB_REPOSITORY_OWNER" --password-stdin <<<"$CI_TOKEN" >/dev/null
if docker pull -q "$image" >/dev/null 2>&1; then
    echo "CI image ${tag}: published, reused"
else
    echo "CI image ${tag}: not published yet, building"
    context="$(mktemp -d)"
    trap 'rm -rf -- "$context"' EXIT
    bash scripts/ci-inputs.sh --list | while IFS= read -r f; do
        mkdir -p "$context/$(dirname "$f")"
        cp -p "$f" "$context/$f"
    done
    docker build --pull -t "$image" -f "$context/ci/Dockerfile" "$context"
    docker push -q "$image"
fi
docker logout "$registry" >/dev/null
[[ -n "${GITHUB_OUTPUT:-}" ]] && echo "image=${image}" >>"$GITHUB_OUTPUT"
echo "$image"
