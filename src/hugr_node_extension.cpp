// hugr_node: duckdb-acl's acl, loaded whole (LoadAclCore - duckdb-acl spec 127), tresor loaded whole (LoadTresorCore -
// tresor spec 024: a platform node's secrets live in the secrets service, spec 002), then the platform part registered
// on acl's store through acl's registration points (duckdb-acl spec 126): the cluster profile, node groups' traits,
// drift and the cluster bundle (acl_cluster.cpp). acl_otel is loaded whole after both (LoadAclOtelCore - acl-otel spec
// 024, spec 005): it attaches to acl's and tresor's hook registries and exports nothing until an endpoint is set. A
// platform node loads this instead of acl, tresor and acl_otel, never beside any of them.
#define DUCKDB_EXTENSION_MAIN

#include "hugr_node_extension.hpp"

#include "acl_cluster.hpp"
#include "acl_extension.hpp"
#include "acl_otel_extension.hpp"
#include "hugr_node_agent.hpp"
#include "hugr_type.hpp"
#include "tresor_extension.hpp"

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

namespace {

void LoadInternal(ExtensionLoader &loader) {
	auto store = LoadAclCore(loader);
	LoadTresorCore(loader);
	LoadAclOtelCore(loader);
	acl::RegisterAclCluster(loader, store);
	hugr_node::RegisterHugrNodeAgent(loader, store); // spec 006: the node process and its bootstrap
	hugr::RegisterHugrType(loader);                  // spec 011: another platform as a source
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
