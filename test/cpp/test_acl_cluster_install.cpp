// Spec 093: ACL CLUSTER INSTALL EXTENSION installs from a trusted repository (duckdb 2.0's CREATE
// EXTENSION REPOSITORY) and writes the item only when the live install worked. Spec 103: the item is
// a version and a repository - the repository's signature, which DuckDB verifies at every INSTALL and
// LOAD, is the integrity check, and a SHA256 clause is refused. The repository is a local directory in duckdb's
// versioned layout
// (<repo>/<name>/<version>/<revision>/<platform>/<name>.duckdb_extension.gz), holding postgres_scanner as DuckDB
// publishes it for this build's version and platform (extensions.duckdb.org - DuckDB v2.0.0 is released, spec 019):
// every build runs the test, not only one that makes the scanner. Skipped when it cannot be fetched (offline).
// Build + run via `GEN=ninja make test-cpp`.

#include "acl_test_util.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <cstdlib>
#include <unistd.h>

using namespace duckdb;
using namespace acl_test;

namespace {

//! DuckDB's own repository: what kista mirrors
const char *const PUBLISHED = "https://extensions.duckdb.org";

//! A valid RSA public key (duckdb's own repository test key): the repository needs one pinned; the
//! loadable is unsigned, which allow_unsigned_extensions admits in this test only
const char *const PUBLIC_KEY =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAt9+KeDkWJa7SiBb7AMdXAPV2qpRunztoC2c7DtRmN9+Lq2YQm4rMQmmAQcwyxTxcK8vwPK"
    "OiE4mLMwmQMvTty9tL+buREfvg5KJ+FY3Bv05cZYsf7VuYgUDcw41zSEE5W1thh5PiafcrMn7CPF0jN0oFfVvAHB/fAM0yaZGPKs3WYqnHAHa6v/cV"
    "MeiWZ/lQORCBqqL2hii1fEiuIqw6AOEoZVsN0/lqm7GfnD899zrzLOPRuP4YFg46drL1UyNdg+4TquJ2OV9kR4CAfdGRexqHFWLmoxkTfnUxJ4yL+g"
    "kV57K1H+smEdKrDhDX68pEFM/34E2eVKZoRU/0paxDZQIDAQAB";

std::string One(Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		return "ERROR: " + result->GetError();
	}
	return result->RowCount() ? result->Collection().GetValue(0, 0).ToString() : "";
}

bool Refused(Connection &con, const std::string &sql, const std::string &why) {
	auto result = con.Query(sql);
	return Check(result->HasError() && result->GetError().find(why) != std::string::npos,
	             "refused (" + why + "): " + (result->HasError() ? result->GetError() : "it ran"));
}

} // namespace

int main() {
	return RunMain("the cluster profile installs from a trusted repository (specs 093, 103)", [] {
		char root[] = "/tmp/acl-cluster-XXXXXX";
		if (!Check(mkdtemp(root) != nullptr, "a directory of the test's own")) {
			return;
		}
		std::string base(root);
		auto dir = base + "/repo/postgres_scanner/v9.9.9/" + ExtensionHelper::GetVersionDirectoryName() + "/" +
		           DuckDB::Platform();
		Check(std::system(("mkdir -p '" + dir + "' '" + base + "/repos' '" + base + "/ext'").c_str()) == 0,
		      "the repository's layout");
		auto url = std::string(PUBLISHED) + "/" + ExtensionHelper::GetVersionDirectoryName() + "/" +
		           DuckDB::Platform() + "/postgres_scanner.duckdb_extension.gz";
		if (std::system(
		        ("curl -fsSL --max-time 120 -o '" + dir + "/postgres_scanner.duckdb_extension.gz' '" + url + "'")
		            .c_str()) != 0) {
			std::cout << "  SKIP: " << url << " could not be fetched\n";
			return;
		}

		DBConfig config;
		config.SetOptionByName("allow_extension_repositories", Value("allowed"));
		config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		config.SetOptionByName("extension_repository_directory", Value(base + "/repos"));
		config.SetOptionByName("extension_directory", Value(base + "/ext"));
		config.SetOptionByName("autoload_known_extensions", Value::BOOLEAN(false));
		DuckDB db(nullptr, &config);
		Connection con(db);
		Exec(con, "CREATE EXTENSION REPOSITORY trusted WITH PREFIX '" + base + "/repo' USING PUBLIC KEY '" +
		              PUBLIC_KEY + "'");
		Exec(con, "ATTACH ':memory:' AS store");
		Exec(con, "SELECT acl_use_db('store','acl',true)");
		Exec(con, "SET GLOBAL acl_allow_anonymous_admin=true");

		Scenario("a SHA256 clause is refused, never ignored, and writes nothing (spec 103)", [&] {
			Refused(con,
			        "ACL ADMIN CLUSTER INSTALL EXTENSION postgres_scanner VERSION 'v9.9.9' FROM trusted SHA256 "
			        "'0000000000000000000000000000000000000000000000000000000000000000'",
			        "SHA256 is no longer part of the cluster profile (spec 103)");
			Check(One(con, "SELECT count(*) FROM acl_cluster_items()") == "0", "the profile is unchanged");
			Check(One(con, "SELECT acl_cluster_version()") == "0", "and so is its version");
		});

		Scenario("the install loads the extension and writes a version and a repository", [&] {
			auto answer =
			    One(con, "ACL ADMIN CLUSTER INSTALL EXTENSION postgres_scanner VERSION 'v9.9.9' FROM trusted");
			Check(answer.find("'applied_here': true") != std::string::npos, "applied on this node: " + answer);
			Check(One(con, "SELECT count(*) FROM duckdb_functions() WHERE function_name = 'postgres_query'") != "0",
			      "loaded: its functions are registered");
			auto spec = One(con, "SELECT spec FROM acl_cluster_items() WHERE name = 'postgres_scanner'");
			Check(spec.find("\"repository\": \"trusted\"") != std::string::npos &&
			          spec.find("\"version\": \"v9.9.9\"") != std::string::npos,
			      "the item names its version and repository: " + spec);
			Check(spec.find("sha256") == std::string::npos, "and no hash: " + spec);
			Refused(con, "ACL ADMIN CLUSTER INSTALL EXTENSION postgres_scanner VERSION 'v9.9.9' FROM trusted",
			        "already in the profile");
			Refused(con, "ACL ADMIN CLUSTER UPDATE EXTENSION postgres_scanner VERSION 'v9.9.10' SHA256 'ab'",
			        "spec 103");
		});

		Scenario("an update keeps the repository and the comment it does not name (spec 019)", [&] {
			One(con, "ACL ADMIN CLUSTER UPDATE EXTENSION postgres_scanner VERSION 'v9.9.9' COMMENT 'the lake''s'");
			Check(One(con, "SELECT comment FROM acl_cluster_items() WHERE name = 'postgres_scanner'") == "the lake's",
			      "COMMENT sets the comment");
			// a second trusted repository: an update naming none keeps the item's, never refused for the choice
			Exec(con, "CREATE EXTENSION REPOSITORY other WITH PREFIX '" + base + "/repo' USING PUBLIC KEY '" +
			              PUBLIC_KEY + "'");
			auto answer = One(con, "ACL ADMIN CLUSTER UPDATE EXTENSION postgres_scanner VERSION 'v9.9.10'");
			Check(answer.find("'class': restart") != std::string::npos, "an update with two repositories: " + answer);
			auto spec = One(con, "SELECT spec FROM acl_cluster_items() WHERE name = 'postgres_scanner'");
			Check(spec.find("\"repository\": \"trusted\"") != std::string::npos &&
			          spec.find("\"version\": \"v9.9.10\"") != std::string::npos,
			      "the item's repository kept, its version moved: " + spec);
			Check(One(con, "SELECT comment FROM acl_cluster_items() WHERE name = 'postgres_scanner'") == "the lake's",
			      "and its comment kept");
			Refused(con, "ACL ADMIN CLUSTER UPDATE EXTENSION postgres_scanner VERSION 'v9.9.11' FROM nowhere",
			        "not a trusted repository");
			Check(One(con, "SELECT spec FROM acl_cluster_items() WHERE name = 'postgres_scanner'") == spec,
			      "a refused update writes nothing");
			One(con, "ACL ADMIN CLUSTER UPDATE EXTENSION postgres_scanner VERSION 'v9.9.10' FROM other");
			Check(One(con, "SELECT spec FROM acl_cluster_items() WHERE name = 'postgres_scanner'")
			              .find("\"repository\": \"other\"") != std::string::npos,
			      "FROM moves the item to another trusted repository");
			// an install still names its repository when the node trusts several
			Refused(con, "ACL ADMIN CLUSTER INSTALL EXTENSION mysql_scanner VERSION 'v1'",
			        "name the repository with FROM");
		});

		Scenario("an update or a removal is restart class: described, not applied live", [&] {
			auto answer = One(con, "ACL ADMIN CLUSTER UPDATE EXTENSION postgres_scanner VERSION 'v9.9.10'");
			Check(answer.find("'class': restart") != std::string::npos &&
			          answer.find("'applied_here': false") != std::string::npos,
			      "update: " + answer);
			answer = One(con, "ACL ADMIN CLUSTER REMOVE EXTENSION postgres_scanner");
			Check(answer.find("'class': restart") != std::string::npos, "remove: " + answer);
			Check(One(con, "SELECT count(*) FROM acl_cluster_items()") == "0", "the item left the profile");
		});
	});
}
