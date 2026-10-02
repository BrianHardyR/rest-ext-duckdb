// rest_namespace_catalog.hpp
//
// Implements "namespace mode": ATTACH ... (resources={"Projects":"https://...","Usage":"https://..."})
// AS gcp (TYPE rest_ext), which makes each resource callable by name and dot-qualified, e.g.
// `SELECT * FROM gcp.Projects(...)` and `SELECT * FROM gcp.Usage(...)`.
//
// --- Why this needs a real Catalog, when single-endpoint mode (see rest_attachment_catalog.hpp)
//     gets away without one ---
//
// DuckDB resolves `some_name(...)` and `some_alias.some_name(...)` completely differently:
//   - A bare, unqualified call like `myapi(...)` is looked up by name across the normal schema
//     search path - it never even looks at what databases are currently ATTACHed.
//   - A dot-qualified call like `gcp.Projects(...)` DOES consult attached-database names: DuckDB
//     sees "gcp", notices it matches something you ATTACHed, and asks THAT attachment's own
//     Catalog "do you have something called Projects?"
//
// So to make `gcp.Projects(...)` resolve at all, `gcp` has to be backed by a real `Catalog`
// object that knows how to answer "here are my schemas" and "here's what's inside this schema" -
// that's what the two classes in this file (kept private to the .cpp; only their factory
// functions below are public) exist to do.
#pragma once

#include "duckdb.hpp"
#include "rest_fetch_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

// One dot-qualified resource inside a namespace, e.g. the "Projects" in gcp.Projects. Each
// resource gets its own full URL (a namespace can span more than one host - GCP's various APIs
// are a good real-world example), its own HTTP method (an imported OpenAPI/Postman spec - see
// spec_import.hpp - mixes GET/POST/etc within one namespace), and its own already-resolved
// headers (see rest_secrets.hpp), since different hosts may match different stored secrets.
struct RestResourceConfig {
	string name;
	string url;
	string method;
	string headers_json;
	RestPagination paging;
};

// Builds the Catalog that backs a namespace-mode ATTACH.
unique_ptr<Catalog> CreateRestNamespaceCatalog(AttachedDatabase &db, vector<RestResourceConfig> resources);

// Every ATTACHed database needs a TransactionManager alongside its Catalog. DuckDB ships a ready-
// made one (DuckTransactionManager), but it only works with its own DuckCatalog implementation -
// it has an internal check that refuses to run against any other kind of Catalog. Since our
// namespace Catalog is NOT a DuckCatalog (it doesn't need real tables, indexes, or on-disk
// storage - just a fixed list of callable resources), we need this small custom one instead. It
// has no real transactional behavior to speak of: everything here is read-only and lives entirely
// in memory, so there's nothing to commit or roll back.
unique_ptr<TransactionManager> CreateRestTransactionManager(AttachedDatabase &db);

} // namespace duckdb
