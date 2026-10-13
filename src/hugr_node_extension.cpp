// hugr_node: duckdb-acl's acl, loaded whole (LoadAclCore - duckdb-acl spec 127), then the platform part registered
// on its store through acl's registration points (spec 126): the cluster profile, node groups' traits, drift, the
// deployment and the cluster bundle (acl_cluster.cpp). A platform node loads this instead of acl, never both.
#define DUCKDB_EXTENSION_MAIN

#include "hugr_node_extension.hpp"

#include "acl_cluster.hpp"
#include "acl_extension.hpp"

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

namespace {

void LoadInternal(ExtensionLoader &loader) {
	auto store = LoadAclCore(loader);
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
