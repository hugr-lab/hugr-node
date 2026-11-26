//===----------------------------------------------------------------------===//
// acl_cluster.hpp
//
// Spec 093: the cluster profile's SQL surface - acl_cluster_extension / _attach / _detach / _setting
// (what `ACL CLUSTER …` compiles to), acl_cluster_version() and acl_cluster_items([scope]). The
// writers themselves are PolicyStore::Cluster* (acl_cluster.cpp).
//===----------------------------------------------------------------------===//

#pragma once

#include "acl_policy.hpp"

#include <set>

namespace duckdb {
class ExtensionLoader;

namespace acl {

void RegisterAclCluster(ExtensionLoader &loader, const shared_ptr<PolicyStore> &store);

//! spec 006: what a node runs to apply one effective profile item at its bootstrap, in the profile's order - the same
//! statements the writing node's hot apply runs (a setting's SET GLOBAL, an extension's INSTALL + LOAD from its
//! repository, a source's ATTACH)
vector<string> ClusterItemApplyStatements(const PolicyStore::ClusterItem &item);

//! The extension a source type needs loaded ('' = a core type): postgres -> postgres_scanner, mysql -> mysql_scanner,
//! a type named like its extension (mssql, ducklake, ...) as itself. spec 010: also the policy catalog's, by its
//! secret's type
string ClusterExtensionOfType(const string &type);

//! spec 010: a hot item this node's own profile write applied here before its commit (an ATTACH, an INSTALL + LOAD, or
//! a database already here that the profile now describes) - the node's tick takes it as its own, so a DETACH written
//! elsewhere detaches it here too. Taken once: the tick reads and clears the notes
void NoteAppliedHere(DatabaseInstance &db, const string &kind, const string &name);
vector<std::pair<string, string>> TakeAppliedHere(DatabaseInstance &db);

//! spec 035 stage 2: a source item's spec without its lazy marks - where the source is (a change of the marks alone
//! re-points nothing); and whether an item is a lazy source, with its idle time in seconds (0 = never detached)
string ClusterSourceIdentity(const string &spec);
bool ClusterSourceLazy(const PolicyStore::ClusterItem &item, int64_t &idle_seconds);
//! spec 035 stage 2: the node's lazy sources, from its effective profile (bootstrap, every tick, its own writes); the
//! trait's attach on demand (duckdb-acl spec 177); the tick's idle detach - what it detached
void RefreshLazySources(DatabaseInstance &db, const vector<PolicyStore::ClusterItem> &items);
vector<string> LazySourceAliases(DatabaseInstance &db);
//! a source the profile drops: out of the registry before its DETACH, so no statement attaches it again meanwhile
void ForgetLazySource(DatabaseInstance &db, const string &alias);
void AttachLazySource(DatabaseInstance &db, const string &alias);
//! `taken`: the sources the tick took as the node's (lowercased) - a database attached by hand is never detached
vector<string> DetachIdleLazySources(DatabaseInstance &db, const std::set<string> &taken);
void RefreshLazySourcesAfterWrite(PolicyStore &store, DatabaseInstance &db);

//! spec 046: the node's secrets services keep the list of the secrets the node may use for 30 s (tresor spec 004), so
//! a secret made a moment ago is no match until then. Asking each attached service for its list refreshes it - done
//! before a source naming a secret is attached, and by the `hugr` type before it refuses a secret it does not see. A
//! service that cannot list keeps its last list (its own rule); nothing here fails
void RefreshSecretsLists(DatabaseInstance &db);
//! whether a profile item is a source that names a secret
bool ClusterSourceNamesSecret(const PolicyStore::ClusterItem &item);

//! spec 047: the administrator's scripts - kind `script`, spec {place, source, sql, on_error}. `place`: before_sources,
//! after_source (of `source`), after_sources, on_session. The node's registry is refreshed with its lazy sources
//! (RefreshLazySources); a place's scripts run in their names' order, each on its own connection of the node's,
//! unprefixed, with `acl_node_group` / `acl_node_label` set as variables first
struct ClusterScriptFailure {
	string name;
	string policy; // fail_node, detach_source, retry (continue never stops)
	string error_class;
};
string ClusterScriptField(const PolicyStore::ClusterItem &item, const char *field);
vector<PolicyStore::ClusterItem> ClusterScriptsAt(DatabaseInstance &db, const string &place,
                                                  const string &source = string());
//! the same choice over a list of items (the effective profile): what the listing and the bootstrap's order read
vector<PolicyStore::ClusterItem> ClusterScriptsIn(const vector<PolicyStore::ClusterItem> &items, const string &place,
                                                  const string &source = string());
//! '' when the script ran, else its error's class (logged with its name and place)
string RunClusterScript(DatabaseInstance &db, const PolicyStore::ClusterItem &script);
//! every script at a place (a source's for after_source); false at the first whose policy stops (not continue). With
//! `queued`, a RETRY failure is queued there and the place goes on (the scripts after the sources)
bool RunClusterScripts(DatabaseInstance &db, const string &place, const string &source, ClusterScriptFailure &failure,
                       vector<ClusterScriptFailure> *queued = nullptr);

//! spec 118: a setting the cluster bundle may put in the profile - the node's resources and tuning (an
//! allowlist); a data path, a trust or audit setting and anything else is passthrough's
bool ClusterBundleMaySet(const string &name);

} // namespace acl
} // namespace duckdb
