// rest_secrets.hpp
//
// Lets a user store header/credential values (like an Authorization bearer token) in DuckDB's own
// secret manager instead of typing them in plaintext inside an ATTACH statement, e.g.:
//
//   CREATE SECRET my_auth (
//       TYPE rest_ext_headers,
//       HEADERS MAP {'Authorization': 'Bearer some-long-token'},
//       SCOPE 'https://api.example.com'
//   );
//
// A "secret" in DuckDB is just a named, storable bag of configuration - the same mechanism used
// for things like S3 credentials. SCOPE says which URLs this secret applies to (matched by
// longest-prefix, so a more specific scope wins over a more general one); this extension's secret
// type is deliberately generic - it's just a map of header name -> header value, not tied to any
// one authentication scheme, so it works equally well for a Bearer token, an API key header,
// Basic auth already base64-encoded, or anything else a REST API might want in its headers.
#pragma once

#include "duckdb.hpp"
#include "rest_ext_compat.hpp"
#ifdef REST_EXT_HAS_EXTENSION_LOADER
#include "duckdb/main/extension/extension_loader.hpp"
#else
#include "duckdb/main/extension_util.hpp"
#endif

namespace duckdb {

// Called once when the extension loads. Teaches DuckDB about the "rest_ext_headers" secret type,
// so `CREATE SECRET (TYPE rest_ext_headers, ...)` becomes valid syntax.
//
// DuckDB >= v1.5.x registers through an ExtensionLoader; DuckDB <= v1.4.x has no such wrapper, so
// this registers straight against the DatabaseInstance via ExtensionUtil instead (see
// rest_ext_compat.hpp).
#ifdef REST_EXT_HAS_EXTENSION_LOADER
void RegisterRestExtHeadersSecretType(ExtensionLoader &loader);
#else
void RegisterRestExtHeadersSecretType(DatabaseInstance &db);
#endif

// Called at ATTACH time for each endpoint we're about to configure. Looks up whichever registered
// "rest_ext_headers" secret best matches `url` (if any), and merges its HEADERS map together with
// `inline_headers_json` (the headers= the user typed directly into the ATTACH string, if any).
//
// If the same header key appears in both places, the inline ATTACH value wins - it's the more
// explicit, more immediate source, so it makes sense for it to take precedence over a secret that
// might be shared across many different ATTACH statements.
//
// Returns a flat JSON object string, in the same shape ParseFlatJsonObject/BuildFlatJsonObject
// (see json_helpers.hpp) already use everywhere else in this extension - callers don't need to
// know or care whether the result came from a secret, inline headers, both, or neither.
string ResolveHeaders(ClientContext &context, const string &url, const string &inline_headers_json);

} // namespace duckdb
