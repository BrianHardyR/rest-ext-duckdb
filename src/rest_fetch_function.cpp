#include "rest_fetch_function.hpp"

#include "http_client.hpp"
#include "json_helpers.hpp"

#include "duckdb/main/client_context.hpp"

#include <cstdlib>

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

// One HTTP response, parsed: its rows, and the cursor for the page after it (both empty on the last
// page). `rows` point INTO `doc`, so the page keeps the document alive.
struct RestPage {
	shared_ptr<yyjson_doc> doc;
	vector<yyjson_val *> rows;
	string next_url;
	string next_token;
	// PAGE_PARAM / OFFSET_PARAM: the page number or offset this page was requested at, and whether
	// another page may follow it (it had rows, and as many as PAGE_SIZE when that is set).
	int64_t position = 0;
	bool more = false;

	bool HasNext() const {
		return !next_url.empty() || !next_token.empty() || more;
	}
};

static yyjson_val *ResolvePointer(yyjson_val *root, const string &pointer) {
	if (pointer.empty()) {
		return root;
	}
	return yyjson_ptr_getn(root, pointer.c_str(), pointer.size());
}

// The body of the next request with the cursor written in at TOKEN_BODY, e.g. Azure Resource
// Graph's {"query": "...", "options": {"$skipToken": "<cursor>"}}.
static string BodyWithToken(const string &body_json, const string &pointer, const string &token) {
	auto source = body_json.empty() ? string("{}") : body_json;
	auto doc = shared_ptr<yyjson_doc>(yyjson_read(source.c_str(), source.size(), 0), yyjson_doc_free);
	if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc.get()))) {
		throw InvalidInputException("rest_ext: TOKEN_BODY needs the request body to be a JSON object");
	}
	auto mut = shared_ptr<yyjson_mut_doc>(yyjson_doc_mut_copy(doc.get(), nullptr), yyjson_mut_doc_free);
	auto value = yyjson_mut_strncpy(mut.get(), token.c_str(), token.size());
	if (!yyjson_mut_doc_ptr_setx(mut.get(), pointer.c_str(), pointer.size(), value, true, nullptr, nullptr)) {
		throw InvalidInputException("rest_ext: TOKEN_BODY \"%s\" is not a pointer the request body can hold",
		                            pointer);
	}
	size_t length = 0;
	auto written = yyjson_mut_write(mut.get(), 0, &length);
	string result(written, length);
	free(written);
	return result;
}

static string QueryWithToken(const string &query_params_json, const string &param, const string &token) {
	case_insensitive_map_t<string> params;
	ParseFlatJsonObject(query_params_json, [&](const string &key, const string &value) { params[key] = value; });
	params[param] = token;
	return BuildFlatJsonObject(params);
}

static string CursorText(yyjson_val *val, const string &option) {
	if (!val || yyjson_is_null(val)) {
		return string();
	}
	if (yyjson_is_str(val)) {
		return string(yyjson_get_str(val), yyjson_get_len(val));
	}
	if (yyjson_is_int(val)) {
		return std::to_string(yyjson_get_sint(val));
	}
	throw IOException("rest_ext: %s points at a value that is neither a string nor an integer", option);
}

// COLUMNS: rewrites a page's row arrays as objects keyed by the page's column names, so they infer
// and read like any other rows. The rewritten rows live in a new document the page keeps alive.
static void NameTabularRows(RestPage &page, yyjson_val *root, const string &columns_pointer) {
	auto columns = ResolvePointer(root, columns_pointer);
	if (!columns || !yyjson_is_arr(columns)) {
		if (page.rows.empty()) {
			return;
		}
		throw IOException("rest_ext: COLUMNS \"%s\" does not point at an array in the response", columns_pointer);
	}
	vector<string> names;
	size_t idx, max;
	yyjson_val *column;
	yyjson_arr_foreach(columns, idx, max, column) {
		auto name = yyjson_is_obj(column) ? yyjson_obj_get(column, "name") : column;
		if (!name || !yyjson_is_str(name)) {
			throw IOException("rest_ext: COLUMNS entry %llu is neither a name nor an object with a \"name\"",
			                  static_cast<unsigned long long>(idx));
		}
		names.emplace_back(yyjson_get_str(name), yyjson_get_len(name));
	}

	auto mut = shared_ptr<yyjson_mut_doc>(yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
	auto array = yyjson_mut_arr(mut.get());
	yyjson_mut_doc_set_root(mut.get(), array);
	for (auto *row : page.rows) {
		if (!yyjson_is_arr(row)) {
			throw IOException("rest_ext: COLUMNS is set, but a row of the response is not an array");
		}
		auto object = yyjson_mut_obj(mut.get());
		yyjson_val *cell;
		yyjson_arr_foreach(row, idx, max, cell) {
			if (idx >= names.size()) {
				break;
			}
			yyjson_mut_obj_add(object, yyjson_mut_strncpy(mut.get(), names[idx].c_str(), names[idx].size()),
			                   yyjson_val_mut_copy(mut.get(), cell));
		}
		yyjson_mut_arr_append(array, object);
	}
	page.doc = shared_ptr<yyjson_doc>(yyjson_mut_doc_imut_copy(mut.get(), nullptr), yyjson_doc_free);
	page.rows.clear();
	yyjson_val *named;
	yyjson_arr_foreach(yyjson_doc_get_root(page.doc.get()), idx, max, named) {
		page.rows.push_back(named);
	}
}

// Fetches one page. `previous` is the page before it, or nullptr for the first request.
static RestPage FetchPage(const RestFetchInfo &info, const string &query_params_json, const string &body_json,
                          const RestPage *previous) {
	auto &paging = info.paging;
	string response_body;
	int64_t position = 0;
	if (paging.Counts()) {
		// A page number counts up by one from PAGE_START; an offset by the rows already read.
		auto by_page = !paging.page_param.empty();
		position = !previous ? (by_page ? paging.page_start : 0)
		                     : previous->position + (by_page ? 1 : static_cast<int64_t>(previous->rows.size()));
		response_body = PerformRestCall(
		    info.url, info.method, info.headers_json,
		    QueryWithToken(query_params_json, by_page ? paging.page_param : paging.offset_param, std::to_string(position)),
		    body_json);
	} else if (!previous) {
		response_body = PerformRestCall(info.url, info.method, info.headers_json, query_params_json, body_json);
	} else if (!previous->next_url.empty()) {
		// A next-page link from the response is only followed on the endpoint's own origin, so a
		// response can't send the request's headers (its credentials) anywhere else.
		if (RestUrlOrigin(previous->next_url) != RestUrlOrigin(info.url)) {
			throw IOException("rest_ext: NEXT_URL \"%s\" is not on the endpoint's origin; not following it",
			                  previous->next_url);
		}
		response_body = PerformRestCallToUrl(previous->next_url, info.method, info.headers_json, body_json);
	} else if (!paging.token_body.empty()) {
		response_body = PerformRestCall(info.url, info.method, info.headers_json, query_params_json,
		                                BodyWithToken(body_json, paging.token_body, previous->next_token));
	} else {
		response_body =
		    PerformRestCall(info.url, info.method, info.headers_json,
		                    QueryWithToken(query_params_json, paging.token_param, previous->next_token), body_json);
	}

	RestPage page;
	page.doc = shared_ptr<yyjson_doc>(yyjson_read(response_body.c_str(), response_body.size(), 0), yyjson_doc_free);
	auto root = page.doc ? yyjson_doc_get_root(page.doc.get()) : nullptr;
	if (!root) {
		throw IOException("rest_ext: a paginated endpoint returned a response that is not JSON");
	}
	auto items = ResolvePointer(root, paging.items);
	if (items && yyjson_is_arr(items)) {
		size_t idx, max;
		yyjson_val *elem;
		yyjson_arr_foreach(items, idx, max, elem) {
			page.rows.push_back(elem);
		}
	} else if (items && !yyjson_is_null(items)) {
		page.rows.push_back(items);
	}
	if (!paging.next_url.empty()) {
		page.next_url = CursorText(ResolvePointer(root, paging.next_url), "NEXT_URL");
	} else if (!paging.next_token.empty()) {
		page.next_token = CursorText(ResolvePointer(root, paging.next_token), "NEXT_TOKEN");
	} else if (paging.Counts()) {
		page.position = position;
		page.more = !page.rows.empty() && (paging.page_size == 0 || page.rows.size() >= paging.page_size);
	}
	if (!paging.columns.empty()) {
		// Last: this replaces page.doc, which `root` points into.
		NameTabularRows(page, root, paging.columns);
	}
	if (!paging.Counts() && previous && page.HasNext() && page.next_url == previous->next_url &&
	    page.next_token == previous->next_token) {
		throw IOException("rest_ext: the API returned the same next-page cursor twice; stopping");
	}
	return page;
}

static LogicalType InferRowType(const vector<yyjson_val *> &rows) {
	auto inferred = LogicalType(LogicalType::SQLNULL);
	for (auto *row : rows) {
		inferred = MergeJsonTypes(inferred, InferJsonType(row));
	}
	return inferred;
}

// Everything BIND figures out and hands forward to EXECUTE. DuckDB calls this our "bind data" -
// every table function that needs to remember something between bind and execute has one of
// these, subclassing the generic FunctionData base class.
struct RestFetchBindData : public FunctionData {
	// The first page of the response (the only page, for an API that doesn't paginate). Its rows
	// point INTO its parsed document, which the page keeps alive - see RestPage.
	RestPage first_page;

	// How many requests bind made to find a page with rows in it (see RestFetchBind), so the scan
	// counts them against MAX_PAGES too.
	idx_t pages_fetched = 0;

	RestPagination paging;

	// With a declared schema, bind requests nothing and the scan fetches the first page.
	bool first_page_deferred = false;

	// The endpoint as ATTACH configured it, for the scan's next-page requests.
	RestFetchInfo request;

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
		result->first_page = first_page;
		result->pages_fetched = pages_fetched;
		result->paging = paging;
		result->request = request;
		result->first_page_deferred = first_page_deferred;
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
		       query_params_json == other.query_params_json && body_json == other.body_json && paging == other.paging;
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

	result->paging = info.paging;
	result->request.url = info.url;
	result->request.headers_json = info.headers_json;
	result->request.method = info.method;
	result->request.paging = info.paging;

	LogicalType inferred;
	if (!info.paging.ShapesResponse()) {
		// This is the actual network call - see http_client.hpp. We do it HERE, in bind,
		// specifically so we can look at the real response and decide the output schema before
		// DuckDB needs it.
		auto response_body = PerformRestCall(result->url, result->method, result->headers_json,
		                                     result->query_params_json, result->body_json);
		result->pages_fetched = 1;
		auto &page = result->first_page;
		page.doc =
		    shared_ptr<yyjson_doc>(yyjson_read(response_body.c_str(), response_body.size(), 0), yyjson_doc_free);
		auto root = page.doc ? yyjson_doc_get_root(page.doc.get()) : nullptr;

		if (!root) {
			// Not JSON (or an empty body) - fall back to the raw response text rather than
			// erroring, since not every REST endpoint returns JSON.
			result->is_raw_text = true;
			result->raw_text = std::move(response_body);
			names.push_back("result");
			return_types.push_back(LogicalType::VARCHAR);
			return std::move(result);
		}

		if (yyjson_is_arr(root)) {
			// The response was a JSON array - each element becomes its own output ROW, typed by
			// merging every element's type (see json_helpers.hpp's MergeJsonTypes).
			size_t idx, max;
			yyjson_val *elem;
			yyjson_arr_foreach(root, idx, max, elem) {
				page.rows.push_back(elem);
			}
		} else {
			// A single JSON value (usually an object) - our one and only output row.
			page.rows.push_back(root);
		}
		inferred = InferRowType(page.rows);
	} else if (!info.paging.column_types.empty()) {
		// A declared schema needs no response to bind, so the first page waits for the scan.
		result->first_page_deferred = true;
		inferred = LogicalType::STRUCT(child_list_t<LogicalType>(info.paging.column_types.begin(),
		                                                         info.paging.column_types.end()));
	} else {
		// A paginated endpoint streams: the schema comes from the first page that has rows, and
		// later pages are fetched only as the scan reaches them (so LIMIT stops early and memory
		// holds one page). A later row that doesn't fit that schema is an error, not a NULL.
		result->first_page = FetchPage(info, result->query_params_json, result->body_json, nullptr);
		result->pages_fetched = 1;
		while (result->first_page.rows.empty() && result->first_page.HasNext()) {
			if (result->pages_fetched >= info.paging.max_pages) {
				throw IOException("rest_ext: stopped after MAX_PAGES (%llu) pages", info.paging.max_pages);
			}
			if (context.interrupted) {
				throw InterruptException();
			}
			auto next = FetchPage(info, result->query_params_json, result->body_json, &result->first_page);
			result->first_page = std::move(next);
			result->pages_fetched++;
		}
		inferred = InferRowType(result->first_page.rows);
	}
	if (inferred.id() == LogicalTypeId::SQLNULL) {
		inferred = LogicalType::VARCHAR; // no rows - nothing to infer from
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

// Per-query state for EXECUTE: the page being read and how far into it we are. It starts at the
// page bind fetched and moves to the next one when that runs out.
struct RestFetchGlobalState : public GlobalTableFunctionState {
	RestPage page;
	idx_t row_idx = 0;
	idx_t pages_fetched = 0;
	bool raw_text_done = false;
};

unique_ptr<GlobalTableFunctionState> RestFetchInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<RestFetchBindData>();
	auto state = make_uniq<RestFetchGlobalState>();
	if (bind.first_page_deferred) {
		state->page = FetchPage(bind.request, bind.query_params_json, bind.body_json, nullptr);
		state->pages_fetched = 1;
	} else {
		state->page = bind.first_page;
		state->pages_fetched = bind.pages_fetched;
	}
	return std::move(state);
}

// Moves to the next non-empty page; false once there are no more.
static bool AdvancePage(ClientContext &context, const RestFetchBindData &bind, RestFetchGlobalState &state) {
	while (state.page.HasNext()) {
		if (state.pages_fetched >= bind.paging.max_pages) {
			throw IOException("rest_ext: stopped after MAX_PAGES (%llu) pages", bind.paging.max_pages);
		}
		if (context.interrupted) {
			throw InterruptException();
		}
		auto next = FetchPage(bind.request, bind.query_params_json, bind.body_json, &state.page);
		state.page = std::move(next);
		state.row_idx = 0;
		state.pages_fetched++;
		if (!state.page.rows.empty()) {
			return true;
		}
	}
	return false;
}

void RestFetchFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<RestFetchBindData>();
	auto &state = data.global_state->Cast<RestFetchGlobalState>();

	if (bind.is_raw_text) {
		// Non-JSON fallback: exactly one row, one column, then we're done. A table function
		// signals "no more rows" by leaving the output chunk at cardinality 0.
		if (state.raw_text_done) {
			output.SetCardinality(0);
			return;
		}
		state.raw_text_done = true;
		output.SetCardinality(1);
		output.SetValue(0, 0, Value(bind.raw_text));
		return;
	}

	auto &column_types = StructType::GetChildTypes(bind.row_type);
	idx_t count = 0;
	// Hand back up to STANDARD_VECTOR_SIZE rows this call; DuckDB will call us again for more if
	// there are any left, and we'll pick up where `state.row_idx` left off.
	while (count < STANDARD_VECTOR_SIZE) {
		if (state.row_idx >= state.page.rows.size()) {
			if (count > 0 || !AdvancePage(context, bind, state)) {
				break;
			}
		}
		auto *row_val = state.page.rows[state.row_idx];
		if (bind.wrap_scalar) {
			// Only one column ("value"), and the JSON value itself IS that column's value.
			output.SetValue(0, count, JsonToValue(row_val, column_types[0].second));
		} else {
			// Convert the whole JSON object into a STRUCT Value matching `bind.row_type` (looking
			// each column up by field name), then unpack its fields into the output columns.
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
