# Navi5 — the two console identities. SPDX-License-Identifier: GPL-3.0-or-later
"""PROD is the published Media title; only `build.sh --release` carries it.
DEV is every other build: its own title, game category 0 so Remote Play shows
frames, and its own /download0 data. Installing or updating DEV never touches
PROD. DEV is local-only: never publish, reserve or submit it."""

PROD = {
    "titleId": "NAVI00001",
    "contentId": "UP9000-NAVI00001_00-NAVI500000000000",
    "conceptId": "00001",
    "applicationCategoryType": 65536,
}
DEV = {
    "titleId": "NAVD00001",
    "contentId": "UP9000-NAVD00001_00-NAVI5DEV00000000",
    "conceptId": "90001",
    "applicationCategoryType": 0,
}
DEV_NAME = "Navi5 Dev"
KEYS = tuple(PROD)


def matches(param, identity):
    return all(param.get(k) == identity[k] for k in KEYS)
