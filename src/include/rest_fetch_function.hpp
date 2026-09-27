// rest_fetch_function.hpp
//
// This file defines what happens when a user actually calls one of our REST-backed table
// functions, e.g. `SELECT * FROM myapi('{"q":"1"}', '{}')`. It's used by BOTH ways of attaching an
// endpoint (see rest_attachment_catalog.hpp for the single-endpoint case and
// rest_namespace_catalog.hpp for the multi-resource case) - they each build their own
// `TableFunction` object, but both point it at the same bind/execute callbacks defined here.
//
// --- A quick primer on how a DuckDB table function works ---
//
// A "table function" is anything you can put in a FROM clause that isn't a plain table, e.g.
// `range(10)` or `read_csv('file.csv')`. DuckDB always calls into one in two separate phases:
//
//   1. BIND happens once, before the query runs at all. Its job is to answer "what COLUMNS will
//      this produce, and what are their TYPES?" - DuckDB needs this answer up front to plan the
//      rest of the query (e.g. to check `SELECT foo FROM my_function()` actually has a `foo`
//      column). Whatever the bind function computes can be stashed away in a small object (our
//      RestFetchBindData) that DuckDB hands back to us later.
//
//   2. EXECUTE happens one or more times afterwards, to actually produce the rows. DuckDB asks for
//      rows in batches (a "DataChunk" at a time, up to STANDARD_VECTOR_SIZE rows each) until we
//      signal we're done by returning an empty batch.
//
// Our twist: instead of hard-coding what columns we'll produce, we make the REAL HTTP request
// during BIND, look at the actual JSON we got back, and use that to decide the columns/types on
// the fly (see json_helpers.hpp for how that inference works). By the time EXECUTE runs, we
// already have the parsed response sitting in RestFetchBindData and just need to convert it into
// DuckDB rows.
#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// The per-endpoint configuration (url/headers/method) that ATTACH decided on. Both attach modes
// build one of these per callable resource and attach it to the TableFunction they construct via
// `TableFunction::function_info` (see the .cpp file for why it has to be threaded through this
// way rather than just captured in a lambda).
struct RestFetchInfo : public TableFunctionInfo {
	string url;
	string headers_json;
	string method;
};

// The BIND callback: makes the real HTTP request, parses the JSON response, and works out the
// output schema (return_types/names) from its shape.
unique_ptr<FunctionData> RestFetchBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names);

// The "set up per-query state" callback, called once per query right before EXECUTE starts. We
// don't need much state (just a row counter), but DuckDB's TableFunction always wants one of
// these if you're going to produce more than a trivial fixed result.
unique_ptr<GlobalTableFunctionState> RestFetchInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// The EXECUTE callback: converts the already-fetched-and-parsed JSON (stashed in bind data) into
// actual DuckDB rows, one batch (DataChunk) at a time.
void RestFetchFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output);

} // namespace duckdb
