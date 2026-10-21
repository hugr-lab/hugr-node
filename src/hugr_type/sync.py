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
# duckdb's patches for quack: hugr-node's duckdb's, which must be duckdb-acl's pin (checked in main()); applied only
# when duckdb's own quack.cmake applies them (v2.0.0's carries none, as duckdb-acl's sync reads it, its spec 141)
DUCKDB_PATCHES = ROOT / "duckdb" / ".github" / "patches" / "extensions" / "quack"
DUCKDB_QUACK_CONFIG = ROOT / "duckdb" / ".github" / "config" / "extensions" / "quack.cmake"
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
    # would look the http secrets for B's URL up as that statement's user. Read for the `hugr://` scope, so an http
    # secret scoped to https:// (a bearer token, extra headers, verify_ssl) is never sent to another platform
    ("hugr_client.cpp",
     "\t\tauto request_url = uri.Http() + \"/quack\";\n"
     "\t\tif (context && context->transaction.HasActiveTransaction()) {\n"
     "\t\t\thttp_params = http_util.InitializeParameters(*context, request_url);\n"
     "\t\t} else {\n"
     "\t\t\thttp_params = http_util.InitializeParameters(db, request_url);\n"
     "\t\t}\n",
     "\t\t// the node's own parameters (a connection to B is the node's, never a statement's), for the `hugr://`\n"
     "\t\t// scope: an http secret scoped to https:// - a bearer token, extra headers - is for other servers\n"
     "\t\thttp_params = http_util.InitializeParameters(db, \"hugr://\" + uri.Host() + \":\" + to_string(uri.Port()));\n"),
    # a redirect would re-send the CONNECT - the token - elsewhere, in cleartext too; B's TLS is never turned off
    ("hugr_client.cpp",
     "\thttp_params->timeout = HTTP_TIMEOUT_SECONDS;\n\thttp_params->retries = 0;\n",
     "\thttp_params->timeout = HTTP_TIMEOUT_SECONDS;\n\thttp_params->retries = 0;\n"
     "\thttp_params->follow_location = false;\n\thttp_params->override_verify_ssl = false;\n"),
    # a log names the connection by its node's label only: the id is the session on B (spec 134), and a log is read
    # by more than the operator
    ("hugr_client.cpp",
     "\tauto msg = QuackLogType::ConstructLogMessage(request_type, connection_id, string(), client_query_id, query,\n",
     "\tauto dot = connection_id.find('.');\n"
     "\tauto label = dot == string::npos ? string() : connection_id.substr(0, dot);\n"
     "\tauto msg = QuackLogType::ConstructLogMessage(request_type, label, string(), client_query_id, query,\n"),
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
    # a heartbeat on a client of its own, bounded like the final DISCONNECT (2 s, no retry): a half-open B fails it in
    # seconds - the pool then replaces the connection - instead of holding the heartbeat thread, and every teardown
    # that joins it, for the request timeout (a day)
    ("hugr_client.cpp",
     "\tauto client = TakeClient(nullptr);\n"
     "\tclient->Request<SuccessResponse>(nullptr, make_uniq<HeartbeatRequestMessage>(connection_id));\n"
     "\tStoreClient(std::move(client));\n",
     "\ttry {\n"
     "\t\tauto client = HugrClient::GetClient(db, uri);\n"
     "\t\tclient->SetAffinity(affinity);\n"
     "\t\tclient->PrepareTeardownRequest();\n"
     "\t\tclient->Request<SuccessResponse>(nullptr, make_uniq<HeartbeatRequestMessage>(connection_id));\n"
     "\t\theartbeat_failed = false;\n"
     "\t} catch (...) {\n"
     "\t\theartbeat_failed = true;\n"
     "\t\tthrow;\n"
     "\t}\n"),
    # a CANCEL that finds no cached client takes a new one - it must never silently do nothing - and holds the cache's
    # lock only to take and give back the client, not across the request
    ("hugr_client.cpp",
     "\tlock_guard<mutex> guard(lock);\n"
     "\tif (cached_clients.empty()) {\n"
     "\t\treturn;\n"
     "\t}\n"
     "\t// get a cached client, if any\n"
     "\tauto &client = cached_clients.back();\n"
     "\tclient->Request<SuccessResponse>(nullptr, make_uniq<CancelRequestMessage>(connection_id, query_uuid));\n",
     "\tauto client = TakeClient(nullptr);\n"
     "\tclient->Request<SuccessResponse>(nullptr, make_uniq<CancelRequestMessage>(connection_id, query_uuid));\n"
     "\tStoreClient(std::move(client));\n"),
    # a FETCH that failed before the constructor's first top-up was done makes Register throw out of the constructor:
    # the destructor then never runs, and the queue (destroyed last) would wait for tasks that touch the members
    # destroyed before it. Drain first
    ("hugr_client_fetch_ahead.cpp",
     "\tTopUp(context);\n}\n\nHugrFetcher::~HugrFetcher() {",
     "\ttry {\n\t\tTopUp(context);\n\t} catch (...) {\n\t\tStopAndDrain();\n\t\tthrow;\n\t}\n}\n\n"
     "HugrFetcher::~HugrFetcher() {"),
]

BANNER = ("// GENERATED by src/hugr_type/sync.py from duckdb-acl's third_party/quack ({commit}) - do not edit.\n"
          "// The `hugr` type's copy of quack's client transport (spec 011): names *Quack* -> Hugr*, the settings it\n"
          "// reads quack_* -> hugr_*; every other string as quack wrote it.\n")

QUACK_NAME = re.compile(r"\b\w*Quack\w*\b")
INCLUDE = re.compile(r'^#include "(quack_[^"]+)"', re.M)
STRING = re.compile(r'"(?:[^"\\\n]|\\.)*"')

# The only text renamed inside a string literal: the settings the copies read, so a stock quack's setting never
# steers the type (none of them is registered as hugr_*: the type passes its values explicitly). Every other literal
# - the protocol's ("/quack", "X-Quack-Connection-Id", the cookies), the messages - stays as quack wrote it.
SETTINGS = ["quack_fetch_read_ahead", "quack_debug_fetch_delay_ms", "quack_default_client_id",
            "quack_default_heartbeat_timeout"]
# what the protocol needs verbatim, checked in the copies after generation
PROTOCOL_LITERALS = {"hugr_client.cpp": ['"/quack"', '"Set-Cookie"'],
                     "hugr_client_affinity.cpp": ['"X-Quack-Connection-Id"']}


def patched_source(commit):
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="hugr-type-sync-"))
    shutil.copytree(QUACK / "src", scratch / "src")
    duckdb_patches = sorted(DUCKDB_PATCHES.glob("*.patch"))
    if not DUCKDB_QUACK_CONFIG.exists():
        sys.exit(f"sync: no {DUCKDB_QUACK_CONFIG} - is hugr-node's duckdb checked out?")
    if "APPLY_PATCHES" in DUCKDB_QUACK_CONFIG.read_text():
        if not duckdb_patches:
            # a copy made without them would compile against the wrong duckdb and pass the sync
            sys.exit(f"sync: duckdb's quack.cmake applies patches, and none are under {DUCKDB_PATCHES}")
    else:
        # duckdb builds this quack as it is: so do the copies, and a patch left in duckdb's tree is not applied
        if duckdb_patches:
            sys.exit(f"sync: duckdb's quack.cmake applies no patches, yet {DUCKDB_PATCHES} holds some")
        print("duckdb carries no patches for this quack: its src as it is, with ours")
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


def head(path):
    return subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], check=True, capture_output=True,
                          text=True).stdout.strip()


def main():
    # acl's embed is built from duckdb-acl's duckdb pin, the copies against hugr-node's: one duckdb, or the copies
    # and the server they talk to differ (read from duckdb-acl's tree, so its duckdb need not be checked out)
    acl_duckdb = subprocess.run(["git", "-C", str(ACL), "rev-parse", "HEAD:duckdb"], check=True, capture_output=True,
                                text=True).stdout.strip()
    if head(ROOT / "duckdb") != acl_duckdb:
        sys.exit("sync: hugr-node's duckdb and duckdb-acl's pin are not the same commit - re-pin first")
    commit = head(QUACK)[:7]
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
                # names are renamed outside string literals only; inside one, the listed settings only
                pieces, last = [], 0
                for m in STRING.finditer(line):
                    pieces.append(QUACK_NAME.sub(rename, line[last:m.start()]))
                    literal = m.group(0)
                    for setting in SETTINGS:
                        if literal.startswith('"' + setting):
                            literal = '"hugr' + literal[len('"quack'):]
                            seen_settings.add(setting)
                    pieces.append(literal)
                    last = m.end()
                pieces.append(QUACK_NAME.sub(rename, line[last:]))
                out.append("".join(pieces))
            return "".join(out)

        seen_settings = set()
        written = {CLIENT[s]: rewrite(t) for s, t in texts.items()}
        for setting in SETTINGS:
            if setting not in seen_settings:
                sys.exit(f"sync: the copies no longer read {setting} - re-audit SETTINGS")
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

        for target, literals in PROTOCOL_LITERALS.items():
            for literal in literals:
                if literal not in written[target]:
                    sys.exit(f"sync: {target} lost the protocol's {literal}")
        for target, text in written.items():
            out = OUT / target
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(BANNER.format(commit=commit) + text)
            # quack is formatted by its own duckdb pin's .clang-format: normalised to ours (clang-format 11.0.1, the
            # lint job's), as duckdb-acl's sync does, so a regeneration is clean - and comparable in CI
            if shutil.which("clang-format"):
                subprocess.run(["clang-format", "-i", str(out)], check=True)
        print(f"sync: {len(written)} files from quack {commit}; shared: {', '.join(sorted(shared))}")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
