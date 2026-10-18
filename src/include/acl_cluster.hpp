//===----------------------------------------------------------------------===//
// acl_cluster.hpp
//
// Spec 093: the cluster profile's SQL surface - acl_cluster_extension / _attach / _detach / _setting
// (what `ACL CLUSTER …` compiles to), acl_cluster_version() and acl_cluster_items([scope]). The
// writers themselves are PolicyStore::Cluster* (acl_cluster.cpp).
//===----------------------------------------------------------------------===//

#pragma once

#include "acl_policy.hpp"

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

//! spec 118: a setting the cluster bundle may put in the profile - the node's resources and tuning (an
//! allowlist); a data path, a trust or audit setting and anything else is passthrough's
bool ClusterBundleMaySet(const string &name);

} // namespace acl
} // namespace duckdb
