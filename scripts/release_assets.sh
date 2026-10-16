#!/usr/bin/env bash
# The release's assets for one platform (spec 008), from a HUGR_NODE_RELEASE=1 build: the node's CLI and the
# extensions a profile installs from a repository, with manifest.json (each file's name, platform, duckdb version and
# its own version, read from the files themselves) and SHA256SUMS.
#   scripts/release_assets.sh <out dir> [build dir, default build/release]
set -euo pipefail
[ $# -ge 1 ] || { sed -n '2,5p' "$0" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$1"
BUILD="${2:-build/release}"
EXTENSIONS="hugr_node postgres_scanner mssql ducklake"

mkdir -p "$OUT"
files=()
for name in $EXTENSIONS; do
	src="$BUILD/extension/$name/$name.duckdb_extension"
	[ -f "$src" ] || { echo "release_assets: no $src - build with HUGR_NODE_RELEASE=1" >&2; exit 1; }
	cp "$src" "$OUT/"
	files+=("$OUT/$name.duckdb_extension")
done
cp "$BUILD/duckdb" "$OUT/duckdb"

# the CLI is described by what it is built from: the platform and duckdb version every extension next to it carries
python3 - "$HERE/ext_footer.py" "$OUT" "${files[@]}" <<'PY'
import json, subprocess, sys
footer, out, files = sys.argv[1], sys.argv[2], sys.argv[3:]
entries = [json.loads(line) for line in subprocess.run(
    [sys.executable, footer, *files], check=True, capture_output=True, text=True).stdout.splitlines()]
platforms = {e["platform"] for e in entries}
versions = {e["duckdb_version"] for e in entries}
if len(platforms) != 1 or len(versions) != 1:
    sys.exit(f"release_assets: the extensions disagree - platforms {platforms}, duckdb versions {versions}")
manifest = {"platform": platforms.pop(), "duckdb_version": versions.pop(),
            "files": [{"name": "duckdb", "kind": "cli"}] + [dict(e, kind="extension") for e in entries]}
json.dump(manifest, open(f"{out}/manifest.json", "w"), indent=2)
PY
(cd "$OUT" && { command -v sha256sum >/dev/null && sha256sum duckdb *.duckdb_extension manifest.json ||
	shasum -a 256 duckdb *.duckdb_extension manifest.json; } >SHA256SUMS)
echo "release_assets: $(ls "$OUT" | tr '\n' ' ')"
