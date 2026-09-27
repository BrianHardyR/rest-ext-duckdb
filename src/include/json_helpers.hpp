// json_helpers.hpp
//
// Small, self-contained helpers for going back and forth between JSON text (or a parsed JSON
// tree) and DuckDB's own type/value system. Nothing in this file knows about HTTP, ATTACH, or
// table functions - it is pure "JSON <-> DuckDB" plumbing, reused by several other files.
//
// This extension uses a library called "yyjson" to read and write JSON. It comes bundled with
// DuckDB itself (in third_party/yyjson), so we get it for free without adding a new dependency.
// yyjson works with two kinds of documents:
//   - "yyjson_doc" / "yyjson_val"          : READ-ONLY, produced by yyjson_read(). We walk these
//                                            with yyjson_is_obj/yyjson_get_str/etc.
//   - "yyjson_mut_doc" / "yyjson_mut_val"  : MUTABLE (buildable), produced by yyjson_mut_doc_new().
//                                            We use these only when we need to WRITE JSON back out.
#pragma once

#include "duckdb.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "yyjson.hpp"

#include <functional>

namespace duckdb {

// Parses a flat JSON object string, e.g. '{"a":"b","c":1}', and calls `callback(key, value)` once
// per entry, in the order the keys appear in the document.
//
// "Flat" means every value must be a plain string, number, or boolean - nested objects/arrays/null
// are silently skipped, since the only things we ever parse this way (HTTP headers and URL query
// parameters) are naturally flat key-value bags. Numbers are formatted with "%g" so e.g. 52.52
// stays "52.52" instead of turning into "52.520000".
//
// Throws InvalidInputException if `json` isn't valid JSON, or isn't a JSON object at all.
void ParseFlatJsonObject(const string &json, const std::function<void(const string &, const string &)> &callback);

// The inverse of ParseFlatJsonObject: takes a plain string-to-string map and serializes it back
// into a JSON object string, e.g. {"a": "b"} -> '{"a":"b"}'. Used when we need to combine header
// values from more than one source (see rest_secrets.hpp) and hand the merged result back to code
// that expects the usual JSON-string representation.
string BuildFlatJsonObject(const case_insensitive_map_t<string> &entries);

// Looks at a single JSON value and decides what DuckDB column type it should become:
//   JSON object -> STRUCT (one field per JSON key, each field's type inferred the same way)
//   JSON array  -> LIST (element type inferred from the array's own elements, merged together)
//   string      -> VARCHAR
//   true/false  -> BOOLEAN
//   whole number -> BIGINT
//   number with a decimal point -> DOUBLE
//   null / missing -> SQLNULL (a placeholder meaning "we don't know yet"; see MergeJsonTypes)
//
// This is the heart of "dynamic schema inference": instead of the extension author hard-coding
// what columns a REST response has, we look at one real response and figure it out at query time.
LogicalType InferJsonType(duckdb_yyjson::yyjson_val *val);

// When we see the same JSON key (or the same array's elements) more than once - e.g. once per row
// of an array of objects - each occurrence might suggest a slightly different type. This function
// decides what type to use when two inferred types disagree, e.g.:
//   BIGINT and DOUBLE together -> DOUBLE (an integer is a valid double, so widen to fit both)
//   two STRUCTs together       -> a STRUCT with the union of both sets of fields
//   anything else that clashes (e.g. a STRUCT in one row, a plain number in another) -> VARCHAR,
//     as a safe fallback so one unusual row doesn't make the whole query fail.
LogicalType MergeJsonTypes(const LogicalType &a, const LogicalType &b);

// Converts a single already-parsed JSON value into a DuckDB Value of the given `type` (normally a
// type that InferJsonType/MergeJsonTypes previously decided on for this column). If the JSON value
// doesn't actually match what was expected (for example the column was typed VARCHAR because of a
// conflict between rows, but this particular row holds a number, not a string), it falls back to
// re-serializing that value as raw JSON text rather than losing the data.
Value JsonToValue(duckdb_yyjson::yyjson_val *val, const LogicalType &type);

} // namespace duckdb
