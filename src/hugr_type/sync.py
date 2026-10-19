#!/usr/bin/env python3
"""Regenerate the `hugr` type's copy of quack's client TRANSPORT (spec 011).

The `hugr` catalog type - a platform as a source of another - talks to B's door, which is quack's server built into
duckdb-acl (src/quack_embed). Its transport is quack's client, copied from THE SAME commit acl's embed is built from
(duckdb-acl's `third_party/quack` submodule), so client and server speak one protocol version:
- quack_client: the HTTPS client (a POST of a quack message through duckdb's HTTPUtil), the connection (its id, the
  heartbeat lease, the cached HTTP clients, CANCEL, DISCONNECT at the end);
- quack_affinity: the cookie jar and `X-Quack-Connection-Id` a front routes by (duckdb/duckdb-quack#297, carried as
  patches/0001 until quack's pin has it);
- quack_fetch_ahead: the FETCH read-ahead on the ASYNC pool.
The catalog, the scan, the pool and the attach are the type's own (hugr_catalog.cpp, hugr_scan.cpp, hugr_pool.cpp,
hugr_type.cpp): read-only, a snapshot per transaction, a connection lent per scan - too far from quack's to patch.

What this script does, run after a duckdb-acl pin bump (`python3 src/hugr_type/sync.py`):
1. copies quack's `src` to a scratch directory and applies, in order, duckdb's own patches for quack
   (`duckdb/.github/patches/extensions/quack/`, the ones acl's embed gets) and ours (`patches/*.patch`). A patch that
   touches nothing under `src` is skipped (as acl's sync does); one that no longer applies fails the sync.
2. takes CLIENT into src/hugr_type/ under `hugr_client*` names, renaming every identifier holding `Quack` to `Hugr`,
   except the names of the SHARED headers - the ones the copies include and do not copy (the protocol: message,
   log, uri; the server's session state, which the client consults; quack's header-only buffers). Those are
   compiled once, by acl's embed, and stay quack's. String literals `"quack..."` become `"hugr..."` (settings), never
   an include.
3. applies CUTS (a span between two anchors removed) and PATCHES (an anchor replaced), each anchor required exactly
   once: a gone anchor fails the sync.

Never edit the generated files by hand: change CUTS / PATCHES (or the hand-written hugr_*.cpp beside them) and
re-run.
"""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
ACL = ROOT / "duckdb-acl"
QUACK = ACL / "third_party" / "quack"
DUCKDB_PATCHES = ACL / "duckdb" / ".github" / "patches" / "extensions" / "quack"
OUR_PATCHES = ROOT / "src" / "hugr_type" / "patches"
OUT = ROOT / "src" / "hugr_type"

# source (under quack's src/) -> generated (under src/hugr_type/)
CLIENT = {
    "quack_client.cpp": "hugr_client.cpp",
    "quack_affinity.cpp": "hugr_client_affinity.cpp",
    "quack_fetch_ahead.cpp": "hugr_client_fetch_ahead.cpp",
    "include/quack_client.hpp": "include/hugr_client.hpp",
    "include/quack_affinity.hpp": "include/hugr_client_affinity.hpp",
    "include/quack_fetch_ahead.hpp": "include/hugr_client_fetch_ahead.hpp",
}

# The type has its own trust and its own credentials, so the client loses what decides either:
# - the pinned-certificate client: B is trusted by its URL and TLS, as all of the node's https (spec 011);
# - the `quack` secret lookups: the token is the type's, read from the `hugr` secret on the node's own connection, and
#   a lookup made with a statement's context would ask tresor as that statement's user.
CUTS = [
    ("include/hugr_client.hpp",
     "//! HTTPS client that trusts exactly one server certificate",
     "} // namespace duckdb"),
    ("hugr_client.cpp",
     "HugrPinnedHttpsQuackClient::HugrPinnedHttpsQuackClient(",
     "unique_ptr<HugrClient> HugrClient::GetClient(DatabaseInstance &db, const QuackUri &uri) {"),
    ("hugr_client.cpp",
     "//! Resolve the optional EXTRA_HTTP_HEADERS from the `quack` secret",
     "void HugrClientConnection::CancelQuery("),
    ("hugr_client.cpp",
     "\tif (token.empty() || server_uri.SslFingerprint().empty()) {",
     "\tif (token.empty()) {\n\t\tthrow"),
]

PATCHES = [
    ("hugr_client.cpp", '#include <openssl/ssl.h>\n#include <openssl/x509.h>\n\n#include "hugr_ssl_key_generator.hpp"\n',
     ""),
    ("hugr_client.cpp", '#include "hugr_secret.hpp"\n', ""),
    # the node's own HTTP parameters, never a statement's: a connection to B is the node's, and a statement's context
    # would look the http secrets for B's URL up as that statement's user
    ("hugr_client.cpp",
     "\t\tif (context && context->transaction.HasActiveTransaction()) {\n"
     "\t\t\thttp_params = http_util.InitializeParameters(*context, request_url);\n"
     "\t\t} else {\n"
     "\t\t\thttp_params = http_util.InitializeParameters(db, request_url);\n"
     "\t\t}\n",
     "\t\thttp_params = http_util.InitializeParameters(db, request_url);\n"),
    # httplib was the pinned client's own transport; the client left talks through duckdb's HTTPUtil
    ("include/hugr_client.hpp", '\n#include "httplib.hpp"\n', "\n"),
    ("hugr_client.cpp",
     "\t\t// Resolve EXTRA_HTTP_HEADERS from the quack secret once; reused for every request on this client.\n"
     "\t\tLoadExtraHttpHeaders(context, db, uri, extra_headers);\n",
     ""),
    ("hugr_client.cpp",
     "\tif (uri.Ssl() && !uri.SslFingerprint().empty()) {\n"
     "\t\t// pinned: our own TLS client, nothing from httpfs is involved\n"
     "\t\treturn make_uniq<HugrPinnedHttpsQuackClient>(db, uri);\n"
     "\t}\n",
     "\tif (!uri.SslFingerprint().empty()) {\n"
     "\t\tthrow InvalidInputException(\"hugr: a pinned certificate is not accepted - B is trusted by its URL and TLS\");\n"
     "\t}\n"),
    # the token is always the caller's (read from the `hugr` secret); the HTTP clients a connection caches are the
    # link's read-ahead, not the node's async threads
    ("include/hugr_client.hpp",
     "string client_id, idx_t heartbeat_timeout_seconds);",
     "string client_id, idx_t heartbeat_timeout_seconds,\n"
     "\t                                                        idx_t max_clients_cached);"),
    ("hugr_client.cpp",
     "idx_t heartbeat_timeout_seconds) {\n\t// Single choke point",
     "idx_t heartbeat_timeout_seconds,\n"
     "                                                               idx_t max_clients_cached) {\n\t// Single choke point"),
    ("hugr_client.cpp",
     "\t// whatever the caller did not supply comes from the secret manager: the token, and the pinned server\n"
     "\t// certificate fingerprint\n"
     "\tauto server_uri = uri;\n",
     "\tauto server_uri = uri;\n"),
    ("hugr_client.cpp",
     "\tidx_t pool_size = MaxValue<idx_t>(1, (idx_t)TaskScheduler::GetScheduler(context).NumberOfAsyncThreads());\n",
     "\tidx_t pool_size = MaxValue<idx_t>(1, max_clients_cached);\n"),
    # the heartbeat's last result, the cheap check before a loan (spec 011, design/019 §5.3)
    ("include/hugr_client.hpp",
     "\tbool ServerInvalidated() const {\n\t\treturn server_invalidated;\n\t}\n",
     "\tbool ServerInvalidated() const {\n\t\treturn server_invalidated;\n\t}\n"
     "\t//! The last heartbeat failed (the lease may be gone on the server): replace the connection before lending it.\n"
     "\tbool HeartbeatFailed() const {\n\t\treturn heartbeat_failed;\n\t}\n"),
    ("include/hugr_client.hpp",
     "\tmutable atomic<bool> server_invalidated {false};\n",
     "\tmutable atomic<bool> server_invalidated {false};\n\tatomic<bool> heartbeat_failed {false};\n"),
    ("hugr_client.cpp",
     "\tauto client = TakeClient(nullptr);\n"
     "\tclient->Request<SuccessResponse>(nullptr, make_uniq<HeartbeatRequestMessage>(connection_id));\n"
     "\tStoreClient(std::move(client));\n",
     "\ttry {\n"
     "\t\tauto client = TakeClient(nullptr);\n"
     "\t\tclient->Request<SuccessResponse>(nullptr, make_uniq<HeartbeatRequestMessage>(connection_id));\n"
     "\t\tStoreClient(std::move(client));\n"
     "\t\theartbeat_failed = false;\n"
     "\t} catch (...) {\n"
     "\t\theartbeat_failed = true;\n"
     "\t\tthrow;\n"
     "\t}\n"),
]

BANNER = ("// GENERATED by src/hugr_type/sync.py from duckdb-acl's third_party/quack ({commit}) - do not edit.\n"
          "// The `hugr` type's copy of quack's client transport (spec 011): *Quack* -> Hugr*, \"quack...\" -> "
          "\"hugr...\".\n")

QUACK_NAME = re.compile(r"\b\w*Quack\w*\b")
INCLUDE = re.compile(r'^#include "(quack_[^"]+)"', re.M)


def patched_source(commit):
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="hugr-type-sync-"))
    shutil.copytree(QUACK / "src", scratch / "src")
    duckdb_patches = sorted(DUCKDB_PATCHES.glob("*.patch"))
    if not duckdb_patches:
        # a copy made without them would compile against the wrong duckdb and pass the sync
        sys.exit(f"sync: no quack patches under {DUCKDB_PATCHES} - is duckdb-acl's duckdb checked out?")
    for patch in duckdb_patches + sorted(OUR_PATCHES.glob("*.patch")):
        targets = re.findall(r"^\+\+\+ b/(\S+)", patch.read_text(), flags=re.M)
        if targets and not any(t.startswith("src/") for t in targets):
            print(f"skipped {patch.name} (touches only {', '.join(targets)})")
            continue
        try:
            subprocess.run(["patch", "-p1", "-s", "-N", "-d", str(scratch), "-i", str(patch)], check=True)
        except subprocess.CalledProcessError:
            sys.exit(f"sync: {patch.name} no longer applies to quack {commit} - re-audit it")
        print(f"applied {patch.name}")
    return scratch


def main():
    commit = subprocess.run(["git", "-C", str(QUACK), "rev-parse", "--short", "HEAD"], check=True,
                            capture_output=True, text=True).stdout.strip()
    scratch = patched_source(commit)
    try:
        src = scratch / "src"
        copied_headers = {s[len("include/"):] for s in CLIENT if s.startswith("include/")}
        texts = {}
        for source in CLIENT:
            path = src / source
            if not path.exists():
                sys.exit(f"sync: {source} is gone from quack {commit} - re-audit the client's file list")
            texts[source] = path.read_text()

        # the shared headers: what the copies include and do not copy, transitively - minus what the cuts remove
        cut_headers = {"quack_secret.hpp", "quack_ssl_key_generator.hpp"}
        shared, pending = set(), [h for t in texts.values() for h in INCLUDE.findall(t)]
        while pending:
            header = pending.pop()
            if header in copied_headers or header in shared or header in cut_headers:
                continue
            shared.add(header)
            pending.extend(INCLUDE.findall((src / "include" / header).read_text()))
        keep = set()
        for header in shared:
            keep |= set(QUACK_NAME.findall((src / "include" / header).read_text()))

        includes = {s[len("include/"):]: t[len("include/"):] for s, t in CLIENT.items() if s.startswith("include/")}

        def rename(m):
            name = m.group(0)
            if name in keep:
                return name
            if name.startswith("Quack"):
                return "Hugr" + name[len("Quack"):]
            return "Hugr" + name  # HttpsQuackClient -> HugrHttpsQuackClient: one prefix, never a half rename

        def rewrite(text):
            out = []
            for line in text.splitlines(keepends=True):
                m = INCLUDE.match(line)
                if m:
                    header = m.group(1)
                    if header in includes:
                        line = line.replace(header, includes[header])
                    elif header in cut_headers:
                        line = line.replace(header, "hugr_" + header[len("quack_"):])  # removed by a patch below
                    out.append(line)
                    continue
                line = QUACK_NAME.sub(rename, line)
                line = re.sub(r'"quack(\w*)', r'"hugr\1', line)
                out.append(line)
            return "".join(out)

        written = {CLIENT[s]: rewrite(t) for s, t in texts.items()}
        for target, start, end in CUTS:
            text = written[target]
            if text.count(start) != 1 or end not in text[text.find(start):]:
                sys.exit(f"sync: a cut's anchors are gone from {target}: {start[:60]!r} / {end[:60]!r}")
            i = text.index(start)
            j = text.index(end, i)
            written[target] = text[:i] + text[j:]
        for target, anchor, replacement in PATCHES:
            if written[target].count(anchor) != 1:
                sys.exit(f"sync: a patch's anchor is in {target} {written[target].count(anchor)} times, expected 1: "
                         f"{anchor[:70]!r}")
            written[target] = written[target].replace(anchor, replacement)

        # what the copies must not carry any more: a credential path, or a definition the shared code also has
        for target, text in written.items():
            for gone in ("Pinned", "SslKeyGenerator", "HugrSecret", "LoadExtraHttpHeaders", "openssl", "httplib"):
                if gone in text:
                    sys.exit(f"sync: {target} still names {gone} - a cut or a patch no longer covers it")
            for m in INCLUDE.finditer(text):
                if m.group(1) not in shared:
                    sys.exit(f"sync: {target} includes {m.group(1)}, which is neither copied nor shared")
            for name in re.findall(r"^(?:class|struct)\s+(\w+)\s*(?:final\s*)?[:{]", text, flags=re.M):
                if name in keep:
                    sys.exit(f"sync: {target} defines {name}, a name of the shared headers")

        for target, text in written.items():
            out = OUT / target
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(BANNER.format(commit=commit) + text)
        print(f"sync: {len(written)} files from quack {commit}; shared: {', '.join(sorted(shared))}")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
