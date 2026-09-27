#include "rest_fetch_function.hpp"

#include "http_client.hpp"
#include "json_helpers.hpp"

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

// Everything BIND figures out and hands forward to EXECUTE. DuckDB calls this our "bind data" -
// every table function that needs to remember something between bind and execute has one of
// these, subclassing the generic FunctionData base class.
struct RestFetchBindData : public FunctionData {
	// The parsed JSON response, kept alive for as long as this bind data exists. `rows` below
	// points INTO this document, so the document must outlive every row pointer - that's exactly
	// what a shared_ptr with yyjson_doc_free as its deleter gives us for free. This is null when
	// the HTTP response wasn't valid JSON at all (see `is_raw_text` below).
	shared_ptr<yyjson_doc> doc;

	// One entry per output ROW. For a JSON response that was a single object, this has exactly one
	// entry (the whole object). For a JSON response that was an array, this has one entry per
	// array element - which is how a single HTTP call can produce many SQL rows.
	vector<yyjson_val *> rows;

	// The schema we decided on, always represented as a STRUCT (one field per output column) even
	// when there's really just one column - see `wrap_scalar` below.
	LogicalType row_type;

	// True when the JSON row itself isn't an object (e.g. the response was a bare array of numbers
	// or strings, not an array of objects). In that case there's no natural set of "field names" to
	// use as columns, so we expose the whole value as a single column named "value" instead.
	bool wrap_scalar = false;

	// Set when the HTTP response couldn't be parsed as JSON at all (e.g. the server returned plain
	// text or HTML). Rather than treating that as an error, we fall back to returning it as one
	// single text column named "result" - the same shape this extension used before dynamic
	// schema inference existed.
	bool is_raw_text = false;
	string raw_text;

	// The original request parameters. We don't need these for anything except Equals() below -
	// they let us answer "would these two binds have made the exact same request?" without having
	// to compare the (potentially large) parsed JSON documents themselves.
	string url, method, headers_json, query_params_json, body_json;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<RestFetchBindData>();
		result->doc = doc;
		result->rows = rows;
		result->row_type = row_type;
		result->wrap_scalar = wrap_scalar;
		result->is_raw_text = is_raw_text;
		result->raw_text = raw_text;
		result->url = url;
		result->method = method;
		result->headers_json = headers_json;
		result->query_params_json = query_params_json;
		result->body_json = body_json;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<RestFetchBindData>();
		return url == other.url && method == other.method && headers_json == other.headers_json &&
		       query_params_json == other.query_params_json && body_json == other.body_json;
	}
};

unique_ptr<FunctionData> RestFetchBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	// `input.info` is how the per-resource config (url/headers/method) that ATTACH set up reaches
	// us here. It has to travel this way rather than as a captured lambda variable, because
	// table_function_bind_t (the type of this very function) is a plain C function pointer under
	// the hood, and plain function pointers cannot capture anything.
	auto &info = input.info->Cast<RestFetchInfo>();

	auto result = make_uniq<RestFetchBindData>();
	result->url = info.url;
	result->headers_json = info.headers_json;
	result->method = info.method;
	// The two arguments the user actually types in their SQL call, e.g.
	// myapi('{"q":"1"}', '{}') - query params first, then body.
	result->query_params_json = input.inputs[0].ToString();
	result->body_json = input.inputs[1].ToString();

	// This is the actual network call - see http_client.hpp. We do it HERE, in bind, specifically
	// so we can look at the real response and decide the output schema before DuckDB needs it.
	auto response_body =
	    PerformRestCall(result->url, result->method, result->headers_json, result->query_params_json, result->body_json);

	result->doc = shared_ptr<yyjson_doc>(yyjson_read(response_body.c_str(), response_body.size(), 0), yyjson_doc_free);
	auto root = result->doc ? yyjson_doc_get_root(result->doc.get()) : nullptr;

	if (!root) {
		// Not JSON (or an empty body) - fall back to the raw response text rather than erroring,
		// since not every REST endpoint returns JSON.
		result->is_raw_text = true;
		result->raw_text = std::move(response_body);
		names.push_back("result");
		return_types.push_back(LogicalType::VARCHAR);
		return std::move(result);
	}

	LogicalType inferred;
	if (yyjson_is_arr(root)) {
		// The response was a JSON array - each element becomes its own output ROW. We still need
		// ONE type that fits every element, so we infer each element's type and merge them
		// together (see json_helpers.hpp's MergeJsonTypes for how mismatches are handled).
		inferred = LogicalType::SQLNULL;
		size_t idx, max;
		yyjson_val *elem;
		yyjson_arr_foreach(root, idx, max, elem) {
			result->rows.push_back(elem);
			inferred = MergeJsonTypes(inferred, InferJsonType(elem));
		}
		if (inferred.id() == LogicalTypeId::SQLNULL) {
			inferred = LogicalType::VARCHAR; // an empty array - nothing to infer from
		}
	} else {
		// The response was a single JSON value (usually an object) - that becomes our one and
		// only output row.
		result->rows.push_back(root);
		inferred = InferJsonType(root);
	}

	if (inferred.id() == LogicalTypeId::STRUCT) {
		result->row_type = std::move(inferred);
	} else {
		// The rows aren't objects (e.g. a bare array of numbers) - there are no natural field
		// names to use as columns, so wrap the whole thing as one column called "value".
		result->wrap_scalar = true;
		result->row_type = LogicalType::STRUCT({{"value", inferred}});
	}

	// Tell DuckDB what the output columns and types are - this is the entire point of bind.
	for (auto &child : StructType::GetChildTypes(result->row_type)) {
		names.push_back(child.first);
		return_types.push_back(child.second);
	}

	return std::move(result);
}

// Per-query state for EXECUTE: just tracks how far through `bind.rows` we've gotten, since DuckDB
// may call EXECUTE several times (once per batch of up to STANDARD_VECTOR_SIZE rows) before we've
// handed back everything.
struct RestFetchGlobalState : public GlobalTableFunctionState {
	idx_t row_idx = 0;
};

unique_ptr<GlobalTableFunctionState> RestFetchInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<RestFetchGlobalState>();
}

void RestFetchFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<RestFetchBindData>();
	auto &state = data.global_state->Cast<RestFetchGlobalState>();

	if (bind.is_raw_text) {
		// Non-JSON fallback: exactly one row, one column, then we're done. A table function
		// signals "no more rows" by leaving the output chunk at cardinality 0.
		if (state.row_idx > 0) {
			output.SetCardinality(0);
			return;
		}
		state.row_idx = 1;
		output.SetCardinality(1);
		output.SetValue(0, 0, Value(bind.raw_text));
		return;
	}

	auto &column_types = StructType::GetChildTypes(bind.row_type);
	idx_t count = 0;
	// Hand back up to STANDARD_VECTOR_SIZE rows this call; DuckDB will call us again for more if
	// there are any left, and we'll pick up where `state.row_idx` left off.
	while (state.row_idx < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto *row_val = bind.rows[state.row_idx];
		if (bind.wrap_scalar) {
			// Only one column ("value"), and the JSON value itself IS that column's value - no
			// field lookup needed.
			output.SetValue(0, count, JsonToValue(row_val, column_types[0].second));
		} else {
			// Convert the whole JSON object into a DuckDB STRUCT Value matching `bind.row_type`
			// (this looks up each column by field name internally), then unpack that struct's
			// fields into the actual output columns.
			auto row_as_struct = JsonToValue(row_val, bind.row_type);
			auto &fields = StructValue::GetChildren(row_as_struct);
			for (idx_t col = 0; col < fields.size(); col++) {
				output.SetValue(col, count, fields[col]);
			}
		}
		state.row_idx++;
		count++;
	}
	output.SetCardinality(count);
}

} // namespace duckdb
