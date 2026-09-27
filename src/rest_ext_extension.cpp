// rest_ext_extension.cpp
//
// The entry point DuckDB actually loads. Its only job is to register everything else this
// extension provides - the real logic lives in the other files listed below:
//
//   rest_attach.{hpp,cpp}             - makes `ATTACH ... (TYPE rest_ext)` work
//   rest_attachment_catalog.{hpp,cpp} - single-endpoint ATTACH mode (bare `myapi(...)` calls)
//   rest_namespace_catalog.{hpp,cpp}  - namespace ATTACH mode (dot-qualified `gcp.Foo(...)` calls)
//   rest_fetch_function.{hpp,cpp}     - the actual bind/execute logic behind every REST call
//   rest_secrets.{hpp,cpp}            - CREATE SECRET support for header/credential values
//   http_client.{hpp,cpp}             - the low-level HTTP request/response code
//   json_helpers.{hpp,cpp}            - JSON <-> DuckDB type/value conversions
#define DUCKDB_EXTENSION_MAIN

#include "rest_ext_extension.hpp"

#include "rest_attach.hpp"
#include "rest_secrets.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	RegisterRestExtStorageExtension(loader.GetDatabaseInstance());
	RegisterRestExtHeadersSecretType(loader);
}

void RestExtExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string RestExtExtension::Name() {
	return "rest_ext";
}

std::string RestExtExtension::Version() const {
#ifdef EXT_VERSION_REST_EXT
	return EXT_VERSION_REST_EXT;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(rest_ext, loader) {
	duckdb::LoadInternal(loader);
}
}
