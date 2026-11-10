# Public CA bundle

Source: https://curl.se/ca/cacert.pem
Published checksum: https://curl.se/ca/cacert.pem.sha256
Extraction documentation: https://curl.se/docs/caextract.html
Retrieved 2026-11-06 over system-verified HTTPS; published checksum matched.
Mozilla certificate data dated 2026-11-05, converted by mk-ca-bundle.pl 1.35.
SHA-256 (whole downloaded PEM):
`427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285`

Mozilla public root certificate data, MPL-2.0; license accompanies this file
as `ca-bundle-MPL-2.0.txt` in the package (source: app/licenses/).
Original PEM comments and certificate attribution retained. No private roots,
private keys or machine-local trust additions. No executable downloaded.

Smoke build sets NAVI5_CA_BUNDLE=/app0/assets/ca-bundle.pem for both curl
transports. Build copies this asset into the app folder; ShadowMount's app0
mount supplies runtime path. No separate /data provisioning required for this
build. Missing/unreadable bundle fails closed; never disable TLS verification.
Sandbox readability still requires console validation. Source fallback remains
/data/navi5/ca-bundle.pem for builds without this compile-time override.

Update deliberately: retrieve official PEM and checksum over verified HTTPS,
review upstream provenance/license, update pinned hash and this record, rebuild
and run offline transport/package checks. Build/deploy never download trust.
