#!/usr/bin/env bash
# Seed a duckdb extension repository (kista) with a release's extensions (spec 008), for dev and test instances:
# compose, kind, the bootstrap e2e. The tenant, the channel and its key are the caller's setup. This registers each
# file's duckdb version on the instance and on the channel when they are missing, then adds each file as a release;
# kista re-signs at intake.
#   scripts/publish_to_kista.sh <kista binary> <kista config> <tenant/channel> <file.duckdb_extension>...
set -euo pipefail
[ $# -ge 4 ] || { sed -n '2,6p' "$0" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
KISTA="$1"
CONFIG="$2"
CHANNEL="$3"
shift 3

# kista reads KISTA_* variables as its configuration: the caller's environment never configures the instance
ka() { env $(env | sed -n "s/^\(KISTA_[A-Za-z0-9_]*\)=.*/-u \1/p") "$KISTA" admin -config "$CONFIG" "$@"; }
field() { python3 -c 'import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])' "$1" "$2"; }

for file in "$@"; do
	meta="$(python3 "$HERE/ext_footer.py" "$file")"
	name="$(field "$meta" name)"
	version="$(field "$meta" duckdb_version)"
	# a release's builds (vX.Y.Z) are kista's release kind, a commit's its dev kind; a CPP build is served under its
	# exact version, so it needs no C API maximum
	kind=dev
	case "$version" in v[0-9]*.[0-9]*.[0-9]*) kind=release ;; esac
	if ! ka version list | grep -qw -- "$version"; then
		ka version add "$version" -kind "$kind" >/dev/null
	fi
	if ! ka channel versions "$CHANNEL" | grep -qx -- "$version"; then
		ka channel versions "$CHANNEL" -add "$version" >/dev/null
	fi
	ka release add "$CHANNEL" "$file" -name "$name" >/dev/null
	echo "published $name ($(field "$meta" platform), duckdb $version) to $CHANNEL"
done
