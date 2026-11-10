# Generated CLDR/ISO data

PyPI release metadata identified Babel 2.16.0/2.17.0/2.18.0 and pycountry
20.7.x/22.x/23.12.x/24.6.1 as candidates preceding that commit. Tested newest
preceding pair first: **Babel 2.18.0 + pycountry 24.6.1 reproduces tracked
`app/src/app/i18n_cldr.cpp` byte-for-byte** (186 language rows, 27 locales).
No older candidates downloaded after exact match. This establishes sufficient
reproduction inputs, not unique historical installed versions or author environment.
No regeneration migration needed. Package versions are not inferred ISO release IDs.

## Actual inputs

`scripts/source-inputs.json` pins these PyPI source distributions (not wheels):

| Archive | SHA-256 | License/source evidence |
| --- | --- | --- |
| `babel-2.18.0.tar.gz` | `b80b99a14bd085fcacfa15c9165f651fbb3406e66cc603abf11c5750937c992d` | Babel BSD-3-Clause `LICENSE`; included CLDR 47 locale data, Unicode-3.0 `babel/locale-data/LICENSE.unicode` |
| `pycountry-24.6.1.tar.gz` | `b61b3faccea67f87d10c1f2b0fc0be714409e8fcdcc1315613174f6466c10221` | LGPL-2.1-only per `PKG-INFO`; full `LICENSE.txt`; bundled `src/pycountry/databases/iso639-3.json` supplies code rows/order |

Babel `CHANGES.rst` identifies CLDR 47. Its bundled Unicode license is identical
to retained `app/src/app/CLDR-LICENSE.txt`. Verbatim package licenses retained
as `app/licenses/Babel-LICENSE.txt` and `pycountry-LICENSE.txt`; source distributions
retain authors, import scripts, data and other notices. Exact URLs in pins.
Downloads total about 16 MB; hashes agree with HTTPS PyPI metadata, not independent
signature authentication. No new separate CLDR/iso-codes upstream-version claim.
Generator reads Babel locale data and pycountry databases directly; no runtime
Python dependency added to Navi5.

## Offline regression

Place pinned archives in ignored cache alongside other source inputs:

```sh
python3 -B app/tests/host/source_delivery.py toolchain/source-cache
```

Python 3.12+ check verifies all archive hashes, extracts into isolated temporary
scratch with `tarfile` data filter, imports only extracted package roots using
`python -S`/`PYTHONPATH` (no pip/network), runs tracked generator, compares bytes,

No runtime Python dependency. Exact reproduction does not establish unique historical versions.
