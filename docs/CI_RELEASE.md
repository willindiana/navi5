# Manual release build

Workflow is manual only (`workflow_dispatch`), read-only contents permission,
SHA-pinned actions and nonpersistent checkout credentials. No secret references,
release creation or tag automation. Artifact upload defaults to `false`.

After separately approved publication, make `release/beta` the default branch so
Actions discovers this workflow. Dispatch must target `release/beta`; job guard
rejects other refs. Choose **Manual RELEASE artifact build**, **Run workflow**,
branch **release/beta**, `upload_artifacts=false`. No run has been claimed here.

Ubuntu 24.04/LLVM 18 CI differs from local reference Clang/LLD 23.1.5. New CI
outputs are not the previously approved local bytes; review their identity and
complete source kit separately. Upload `true` needs separate approval and keeps
artifacts only seven days, not durable release-source availability.

See [source identity](SOURCE.md), [build](DEVELOPMENT.md) and
[installation](RECIPIENT_RELINK.md#installation-on-supported-setup).
