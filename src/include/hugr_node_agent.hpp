//===----------------------------------------------------------------------===//
// hugr_node_agent.hpp
//
// Spec 006: the node process and its bootstrap. hugr_node_run() builds a platform node from its pod's environment
// and the control plane's bootstrap document, serves, and stops on SIGTERM; hugr_node_bootstrap_plan(document) answers
// the statements the bootstrap would run; hugr_node_profile_statements() the ones that apply this node's profile.
// The plan is pure (the environment and the document in, statements out) - the tests and an operator read it.
//===----------------------------------------------------------------------===//

#pragma once

#include "acl_policy.hpp"

namespace duckdb {
class ExtensionLoader;

namespace hugr_node {

//! The local part: what the pod's environment holds - the way into tresor and the control plane, never a credential
struct AgentEnvironment {
	string tresor;           // HUGR_TRESOR: tresor-server's host (a leading "tresor:" is cut)
	string issuer;           // HUGR_IDENTITY_ISSUER
	string client_id;        // HUGR_IDENTITY_CLIENT_ID
	string scope;            // HUGR_IDENTITY_SCOPE (optional): the OAuth scope of the login
	string assertion_file;   // HUGR_IDENTITY_ASSERTION_FILE: the projected service-account token (FLOW federated)
	string private_key_file; // HUGR_IDENTITY_PRIVATE_KEY_FILE: dev only, private_key_jwt (FLOW client_credentials)
	string key_id;           // HUGR_IDENTITY_KEY_ID (optional, with the private key)
	string control_plane;    // HUGR_CONTROL_PLANE: https base URL
	string ca_file;          // HUGR_CA_FILE (optional): ca_cert_file
	string flight, flight_cert, flight_key;
	string quack, quack_cert, quack_key;
	int64_t drain_deadline = 25; // HUGR_DRAIN_DEADLINE, seconds

	static AgentEnvironment FromProcess();
	//! the same variables as a JSON object (hugr_node_bootstrap_plan's `environment :=`)
	static AgentEnvironment FromJson(const string &json);
	//! refuses what the bootstrap cannot run on, naming the variable
	void Validate() const;
	bool DevIdentity() const {
		return assertion_file.empty() && !private_key_file.empty();
	}
};

//! What the control plane answers at GET <HUGR_CONTROL_PLANE>/v1/bootstrap (version 1): pointers, never a secret
struct BootstrapDocument {
	int64_t version = 0;
	string node_group;
	string repository_name = "hugr";
	string repository_prefix;
	string catalog_type, catalog_extension, catalog_path, catalog_secret, catalog_schema = "acl";

	//! refuses an unknown version, a missing field (named) and a prefix that is not https
	static BootstrapDocument Parse(const string &json);
};

struct PlanStep {
	string step;
	string sql;
};

//! The bootstrap's statements in order, the profile's one placeholder row standing for hugr_node_profile_statements()
vector<PlanStep> BootstrapPlan(const AgentEnvironment &env, const BootstrapDocument &document,
                               const string &quack_token);

void RegisterHugrNodeAgent(ExtensionLoader &loader, const shared_ptr<acl::PolicyStore> &store);

} // namespace hugr_node
} // namespace duckdb
