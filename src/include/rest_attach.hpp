// rest_attach.hpp
//
// This is the file that makes `ATTACH '...' AS myapi (TYPE rest_ext)` work at all. It's the "top"
// of this extension: it parses the ATTACH path string, decides which of the two supported shapes
// the user asked for (see below), and delegates the actual work to the other files in this
// extension.
//
// --- A quick primer on how ATTACH works for a custom extension ---
//
// DuckDB lets an extension register a "StorageExtension" - basically a small struct of two
// function pointers:
//   - `attach`: called once, when the user runs `ATTACH '<path>' AS <name> (TYPE <this_extension>)`.
//     Its job is to return a `Catalog` object representing whatever got attached.
//   - `create_transaction_manager`: called right after `attach`, to build the TransactionManager
//     every attached database needs alongside its Catalog.
//
// This extension supports THREE different shapes of ATTACH, all handled by the same `attach`
// callback (RestAttach, in the .cpp file):
//
//   1. Single-endpoint mode:  ATTACH 'url=... headers=... method=...' AS myapi (TYPE rest_ext)
//      -> makes one bare callable: `myapi(query_params, body)`.
//      See rest_attachment_catalog.hpp for how this one is wired up.
//
//   2. Namespace mode:        ATTACH 'resources={"Foo":"...","Bar":"..."} ...' AS gcp (TYPE rest_ext)
//      -> makes each resource dot-qualified: `gcp.Foo(...)`, `gcp.Bar(...)`.
//      See rest_namespace_catalog.hpp for how this one is wired up.
//
//   3. Spec-import mode:      ATTACH 'spec.json' AS gcp (TYPE rest_ext, FORMAT 'openapi')
//                             ATTACH 'x' AS gcp (TYPE rest_ext, FORMAT 'openapi', SPEC_URL 'https://...')
//      -> same dot-qualified shape as namespace mode, but the resource list is parsed out of an
//      OpenAPI spec instead of being hand-written - either a local file (the ATTACH path) or a
//      remote one (the SPEC_URL option - see spec_import.hpp for why a URL can't just go in the
//      ATTACH path directly).
#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Called once when the extension loads. Registers the "rest_ext" StorageExtension so ATTACH
// statements with `(TYPE rest_ext)` know what to do.
void RegisterRestExtStorageExtension(DatabaseInstance &db);

} // namespace duckdb
