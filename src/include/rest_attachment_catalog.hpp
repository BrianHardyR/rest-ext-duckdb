// rest_attachment_catalog.hpp
//
// Implements "single-endpoint mode": ATTACH 'url=... headers=... method=...' AS myapi (TYPE
// rest_ext), which makes exactly one bare, unqualified callable named after the alias, e.g.
// `SELECT * FROM myapi(...)`.
//
// --- Why this DOESN'T need a real Catalog, unlike namespace mode ---
//
// A bare, unqualified call like `myapi(...)` is resolved by DuckDB purely by NAME - it looks the
// name up across the normal schema search path, and never even checks whether "myapi" happens to
// also be the name of something ATTACHed. So the actual callable table function has to live
// somewhere DuckDB will find it through ordinary name lookup: the shared "system" catalog, which
// is always searched no matter what database is currently active (built-in functions like
// `range()` live there too). See rest_attach.cpp for where that registration actually happens.
//
// ATTACH still mechanically requires SOME Catalog object to be returned, though - DuckDB needs
// something to hang the alias/AttachedDatabase bookkeeping off of, and something for DETACH to
// clean up. That's all this file's Catalog is for: a placeholder that does nothing except make
// sure DETACH removes the function we registered in the system catalog (see OnDetach in the .cpp
// file) - without that, the function would keep answering to its old name forever, even after
// DETACH, and re-ATTACHing the same alias would then fail with a "already exists" error.
#pragma once

#include "duckdb.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

// Builds the placeholder Catalog for single-endpoint mode. `function_name` is the ATTACH alias
// (e.g. "myapi") - the same name the real callable was registered under in the system catalog, so
// OnDetach knows what to remove later.
unique_ptr<Catalog> CreateRestAttachmentCatalog(AttachedDatabase &db, string function_name);

} // namespace duckdb
