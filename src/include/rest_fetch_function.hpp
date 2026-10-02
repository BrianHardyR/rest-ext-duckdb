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
// How to walk a paginated API, from the ATTACH options of the same names (see USAGE.md). Every
// "pointer" is an RFC 6901 JSON pointer into one page's response, e.g. '/data' or
// '/properties/nextLink'. With none of NEXT_URL, NEXT_TOKEN, PAGE_PARAM and OFFSET_PARAM set,
// there is exactly one page.
struct RestPagination {
	string items;        // ITEMS: the array of rows in a page; empty = the whole response
	string next_url;     // NEXT_URL: an absolute URL for the next page, same origin only
	string next_token;   // NEXT_TOKEN: an opaque cursor for the next page...
	string token_body;   // TOKEN_BODY: ...written into the request body at this pointer, or
	string token_param;  // TOKEN_PARAM: ...sent as this query-string parameter
	string page_param;   // PAGE_PARAM: a page number sent as this query-string parameter, counting up from
	int64_t page_start = 0; // PAGE_START (default 0)
	string offset_param; // OFFSET_PARAM: how many rows were read before this page, as this query-string parameter
	// PAGE_SIZE: with PAGE_PARAM or OFFSET_PARAM, a page with fewer rows is the last; without it, only
	// an empty page is.
	idx_t page_size = 0;
	idx_t max_pages = 10000;
	// COLUMNS: the column list of a tabular response ({"columns": [...], "rows": [[...], ...]}); each
	// row array becomes an object keyed by those names. An entry is a name or an object with "name".
	string columns;
	// COLUMN_TYPES: the output schema, declared instead of inferred: exactly these columns, with
	// these types, and no request until the scan starts - so a view or PREPARE costs no call.
	string column_types_json;
	vector<std::pair<string, LogicalType>> column_types;

	// PAGE_PARAM or OFFSET_PARAM: the next request is worked out by counting, not read from the response.
	bool Counts() const {
		return !page_param.empty() || !offset_param.empty();
	}
	bool Paginates() const {
		return !next_url.empty() || !next_token.empty() || Counts();
	}
	// True when any option shapes the response, so rows go through the paged reader.
	bool ShapesResponse() const {
		return Paginates() || !items.empty() || !columns.empty() || !column_types.empty();
	}
	bool operator==(const RestPagination &other) const {
		return items == other.items && next_url == other.next_url && next_token == other.next_token &&
		       token_body == other.token_body && token_param == other.token_param && page_param == other.page_param &&
		       page_start == other.page_start && offset_param == other.offset_param && page_size == other.page_size &&
		       max_pages == other.max_pages && columns == other.columns && column_types_json == other.column_types_json;
	}
};

struct RestFetchInfo : public TableFunctionInfo {
	string url;
	string headers_json;
	string method;
	RestPagination paging;
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
