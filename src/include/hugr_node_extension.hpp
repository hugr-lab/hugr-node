#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! The platform node's extension: duckdb-acl's acl built in, plus the platform part
class HugrNodeExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
