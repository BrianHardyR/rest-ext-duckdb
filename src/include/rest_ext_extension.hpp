#pragma once

#include "duckdb.hpp"
#include "rest_ext_compat.hpp"

namespace duckdb {

class RestExtExtension : public Extension {
public:
	// DuckDB >= v1.5.x takes an ExtensionLoader; DuckDB <= v1.4.x takes the DuckDB wrapper directly
	// (see rest_ext_extension.cpp and rest_ext_compat.hpp).
#ifdef REST_EXT_HAS_EXTENSION_LOADER
	void Load(ExtensionLoader &db) override;
#else
	void Load(DuckDB &db) override;
#endif
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
