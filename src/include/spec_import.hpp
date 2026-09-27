// spec_import.hpp
//
// Lets a user configure a namespace ATTACH straight from an OpenAPI spec document they already
// have, instead of hand-writing a resources={...} mapping - e.g.:
//
//   ATTACH 'petstore-openapi.json' AS petstore (TYPE rest_ext, FORMAT 'openapi');
//   SELECT * FROM petstore.listPets('{}', '{}');
//
// OpenAPI (2.0 "Swagger" and 3.x) is a machine-readable, industry-standard description of an
// API's endpoints, methods, and base URL(s) - most API providers either publish one directly, or
// can generate one from other formats (e.g. a Postman collection can be converted to OpenAPI with
// widely available third-party tools, so this extension only needs to understand one format well
// rather than several partially).
//
// Parsing an OpenAPI document produces exactly what this extension already knows how to use: a
// list of RestResourceConfig entries (see rest_namespace_catalog.hpp) - one per operation, each
// with its own name, URL, and HTTP method. From there, namespace mode works exactly the same as
// it does for a hand-written resources={...} mapping.
#pragma once

#include "duckdb.hpp"
#include "rest_namespace_catalog.hpp"

namespace duckdb {

// Parses `spec_json` (the full text of an OpenAPI document) into a list of callable REST
// resources, one per operation.
//
//   base_url_override - if non-empty, used as the base URL for every operation instead of
//                       whatever the spec's own "servers" (3.x) or "host"+"basePath" (2.0) says.
//                       A 3.x server URL containing {variable} placeholders (e.g.
//                       "https://{environment}.example.com") has each variable substituted with
//                       its own declared default before this override would even apply.
//
//   spec_origin       - the "scheme://host[:port]" the spec document itself was fetched from
//                       (when known - i.e. when it came from a SPEC_URL rather than a local
//                       file), used ONLY to resolve a RELATIVE server URL. OpenAPI explicitly
//                       allows a "servers" entry to be relative (e.g. just "/api/v3"), meaning
//                       "wherever this document itself is hosted" - a real, common pattern (the
//                       official Swagger Petstore v3 spec does exactly this), not a malformed
//                       spec. Pass an empty string when there's no meaningful origin (a spec
//                       loaded from a local file); a relative server URL with no origin and no
//                       base_url_override is a clear error rather than a silently broken URL.
//
// URL templates with {parameter} placeholders (e.g. a path like "/users/{id}") are preserved
// as-is in the resulting resource's URL - see http_client.hpp for how those get filled in from
// the caller's own query_params argument at call time.
//
// Throws InvalidInputException if the document can't be parsed as JSON, doesn't look like an
// OpenAPI document at all, has no usable operations, or no absolute base URL can be determined.
vector<RestResourceConfig> ParseOpenApiSpec(const string &spec_json, const string &base_url_override,
                                            const string &spec_origin);

} // namespace duckdb
