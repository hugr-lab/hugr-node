// hugr_node: duckdb-acl's acl, loaded whole (LoadAclCore - duckdb-acl spec 127), tresor loaded whole (LoadTresorCore -
// tresor spec 024: a platform node's secrets live in the secrets service, spec 002), then the platform part registered
// on acl's store through acl's registration points (duckdb-acl spec 126): the cluster profile, node groups' traits,
// drift and the cluster bundle (acl_cluster.cpp). A platform node loads this instead of acl and tresor, never beside
// either.
#define DUCKDB_EXTENSION_MAIN

#include "hugr_node_extension.hpp"

#include "acl_cluster.hpp"
#include "acl_extension.hpp"
#include "tresor_extension.hpp"

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

namespace {

void LoadInternal(ExtensionLoader &loader) {
	auto store = LoadAclCore(loader);
	LoadTresorCore(loader);
	acl::RegisterAclCluster(loader, store);
}

} // namespace

void HugrNodeExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string HugrNodeExtension::Name() {
	return "hugr_node";
}

std::string HugrNodeExtension::Version() const {
#ifdef EXT_VERSION_HUGR_NODE
	return EXT_VERSION_HUGR_NODE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(hugr_node, loader) {
	duckdb::LoadInternal(loader);
}
}
