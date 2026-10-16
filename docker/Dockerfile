# The platform node's image (spec 008): the node's duckdb CLI - hugr_node, httpfs, azure and icu linked in - and
# nothing else. No key, no secret, no extension beyond the linked ones: the profile's extensions come from the
# repository at start, and the trust settings from the bootstrap (spec 006).
#
# Built by the release workflow on each platform's own runner from that platform's assets
# (scripts/release_assets.sh); the build context is the assets directory:
#   docker build -f docker/Dockerfile --build-arg VERSION=<tag> --build-arg REVISION=<sha> \
#       --build-arg DUCKDB_VERSION=<source id> <assets dir>
#
# ubuntu:24.04 is the glibc the binary was built against (the ubuntu-24.04 runners). tini is PID 1: it forwards
# SIGTERM, so a node stopped during its bootstrap ends at once (its own handler serves only once the doors are open)
# and one stopped while serving drains.
FROM ubuntu:24.04

ARG VERSION=dev
ARG REVISION=unknown
ARG DUCKDB_VERSION=unknown
LABEL org.opencontainers.image.title="hugr-node" \
      org.opencontainers.image.description="The hugr platform node: duckdb with hugr_node built in" \
      org.opencontainers.image.source="https://github.com/hugr-lab/hugr-node" \
      org.opencontainers.image.licenses="BUSL-1.1" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${REVISION}" \
      dev.hugr.duckdb.version="${DUCKDB_VERSION}"

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --user-group --home-dir /var/lib/hugr-node --create-home --shell /usr/sbin/nologin hugr-node

COPY --chmod=0755 duckdb /usr/local/bin/duckdb

# HOME holds what duckdb installs from the repository (~/.duckdb/extensions): the pod's emptyDir
ENV HOME=/var/lib/hugr-node
WORKDIR /var/lib/hugr-node
USER 10001:10001

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/duckdb", "-c", "PRAGMA hugr_node_run"]
