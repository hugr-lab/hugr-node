# Spec 008: the release - the node's image and the profile's extensions

- **Status**: implemented
- **Date**: 2026-10-16

## Summary

A tag `v*` builds what a platform node runs, at the submodule pins rather than the branch head:

- **The image**: the duckdb CLI with hugr_node, httpfs, azure and icu linked, for amd64 and arm64.
- **The profile's extensions**: postgres_scanner, mssql and ducklake, which a profile installs from a repository, plus
  the hugr_node loadable. All are built at the image's duckdb version.

The image goes to ghcr only after the bootstrap e2e (spec 006) has passed on the image itself. The extensions are the
release's assets: a repository takes them in and signs them.

## Problem

- The distribution build compiles the loadable against the head of `v2.0-cyanoptera`. It is an early warning of
  drift, not a release, and nothing builds the node's CLI or an image.
- A profile installs its scanners from a repository, and no build exists for our pin: core and community have none
  until duckdb v2.0.0.
- We have no hosted repository. Phase 1 runs its own kista in compose and kind, seeded in setup; where the platform
  takes our extensions from in production is open decision 7.

## Design

### The workflow (`.github/workflows/release.yml`)

- **Triggers**: a pushed tag `v*`. The dry run, which builds and tests but publishes nothing, comes from
  `workflow_dispatch` and from any pull request that touches the release's machinery (the workflow, `docker/`,
  `scripts/`, `extension_config.cmake`, the bootstrap e2e): a release is proven before it is needed.
- **Matrix**: linux_amd64 on `ubuntu-24.04` and linux_arm64 on `ubuntu-24.04-arm`, both native (the repository is
  public, so the arm runners are free). Each runs `HUGR_NODE_RELEASE=1 GEN=ninja make` at the submodule pins, with
  vcpkg from the family's R2 cache.
- **`HUGR_NODE_RELEASE=1`** in `extension_config.cmake` loads postgres_scanner, mssql and ducklake as **loadables**,
  never linked. Their pins are the ones duckdb-acl tests at its pin, read from `duckdb-acl/extension_config.cmake`
  (spec 131: each at its own main). The release therefore ships the combination acl's suite and its door e2e prove.
- **Assets per platform**:
  - `duckdb`, the node's CLI;
  - `hugr_node`, `postgres_scanner`, `mssql` and `ducklake` as `.duckdb_extension` files, unsigned;
  - `SHA256SUMS`;
  - `manifest.json`: each file's name, version, platform and duckdb source id.

### The image

- **Name**: `ghcr.io/hugr-lab/hugr-node:<tag>`, one manifest list for both platforms.
- **Base**: `ubuntu:24.04`, the glibc the binary was built against, with `ca-certificates`.
- **Layout**: a non-root user; `HOME=/var/lib/hugr-node`, which is the pod's emptyDir; the CLI at
  `/usr/local/bin/duckdb`; `ENTRYPOINT ["duckdb", "-c", "PRAGMA hugr_node_run"]`.
- **Nothing else inside**: no key, no secret, no extension beyond what is linked. The profile's extensions come from
  the repository at start (spec 006).
- **Labels**: `org.opencontainers.image.{source,revision,version}` and the duckdb source id.

### The gate before anything is published

1. On amd64, the bootstrap e2e runs against the image. With `NODE_IMAGE` set, `run.sh` starts the node as
   `docker run --network host`, passing the same `HUGR_*` variables and mounting the CA and dev-key files read-only.
   Its kista is seeded with this build's extensions, not the e2e's own postgres_scanner.
2. On arm64, a smoke test: the image's CLI lists hugr_node, httpfs, azure and icu as loaded in `duckdb_extensions()`.
3. Then the image is pushed (both platforms), and a GitHub Release is created with the assets.
   - Build provenance for the image and the assets comes from `actions/attest-build-provenance`.
   - A dry run stops before the push.

### Seeding a repository

`scripts/publish_to_kista.sh <assets> <kista config> <tenant/channel>` runs the `kista admin` calls the e2e makes
(version add, channel versions, release add) for every extension in a release. It is for dev and test instances
(compose, kind), and kista re-signs at intake.

Trusted publishing to a hosted repository is a job that waits for decision 7.

## Enforcement & security

- **No credential in the image.** The bootstrap sets the trust settings (spec 006).
- **Unsigned assets are safe to publish.** A node loads only what a repository it trusts signed, because duckdb
  verifies the signature. A repository's intake checks the footer's version and platform, the ELF and the entry
  point, and then re-signs (kista).
- **Releases run on a tag push only**, never on a pull request. The push uses `GITHUB_TOKEN` with
  `packages: write` and `attestations: write`. The only other secrets are the vcpkg cache's.

## Testing

- The dry run on this spec's pull request: both platforms build, the e2e passes against the image, the arm64 smoke
  test passes, and nothing is pushed.
- The e2e's `NODE_IMAGE` mode on CI. Locally the binary path stays as it is, because macOS builds no Linux image.

## As built

- **The dry run passed on the PR** (both platforms).
  - amd64: the image bootstrapped itself against a kista seeded with the four extensions, which kista's intake
    accepted (linux_amd64, duckdb 7304f7b115). It served, drained on SIGTERM through docker and tini with exit 0, and
    the broken step stopped it.
  - arm64: the suite passed, and the image listed its linked extensions.
- **Found while building it**:
  - **The pins.** `duckdb_extension_load` called inside a CMake *function* registers in the function's scope, so the
    scanners were announced ("Load extension …") and never built. The loader is a macro.
  - **The key's mode.** tresor refuses a key that others may read, and the e2e's first pass made the key 0644 so the
    image's user could read it. The image's e2e now reads the key as a pod's fsGroup would: 0640 plus
    `--group-add`.
- **`workflow_dispatch` needs the workflow on the default branch.** Until the merge, the dry run comes from the pull
  request's own trigger.

## Open decisions (owner, decided 2026-10-16)

| # | question | proposal |
| --- | --- | --- |
| 1 | Where the release's extensions go in phase 1 | the release's assets, with our test kistas seeded from them; trusted publishing once a hosted repository exists (decision 7) |
| 2 | Base and glibc | `ubuntu:24.04`, built natively on `ubuntu-24.04`; manylinux_2_28 only if an older host is needed |
| 3 | Image name | `ghcr.io/hugr-lab/hugr-node` |

## Alternatives considered

- **The reusable distribution workflow for the release.** It builds against the branch head, uploads only the main
  extension, and does not keep the CLI.
- **Building inside a Dockerfile.** That needs vcpkg and the cache's credentials inside `docker build`, and it cannot
  share the runner's cache.
- **QEMU for arm64.** Many times slower, when native arm runners exist.

## Follow-ups

- The dev compose (the image with Keycloak, tresor-server, Postgres and kista) comes with the control plane
  (hugr-platform 001), where a real bootstrap document exists.
