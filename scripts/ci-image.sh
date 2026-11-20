#!/usr/bin/env bash
# Navi5 — make sure the CI job image for the current ci/ exists, and name it.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The tag is a hash of every file in ci/, so unchanged dependencies reuse the
# published image at once; a change builds and pushes a new tag. The registry is
# the Forgejo instance running the job (taken from GITHUB_SERVER_URL, never
# written into the repository). Needs DOCKER_HOST (the runner's Docker daemon)
# and CI_TOKEN, the owner's shared package token (PACKAGES_TOKEN secret).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

tag="$(find ci -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)"
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
    docker build --pull -t "$image" ci
    docker push -q "$image"
fi
docker logout "$registry" >/dev/null
[[ -n "${GITHUB_OUTPUT:-}" ]] && echo "image=${image}" >>"$GITHUB_OUTPUT"
echo "$image"
