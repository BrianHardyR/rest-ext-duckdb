#include "http_client.hpp"

#include "json_helpers.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/http_util.hpp"

// This file builds its own OpenSSL-enabled instantiation of the vendored, header-only httplib
// library (it lives at duckdb/third_party/httplib, bundled with DuckDB itself - not something we
// added). Defining CPPHTTPLIB_OPENSSL_SUPPORT before including it is what turns on HTTPS support;
// DuckDB core's own (separate) use of this same header does NOT define this macro, so linking
// OpenSSL in CMakeLists.txt alone would not be enough without this #define here too.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

namespace duckdb {

// Replaces every {name} placeholder in `url_template` with the matching entry from `params`,
// REMOVING each one it uses - the caller passes what's left in `params` afterwards to become the
// actual "?key=value" query string, so a value used to fill in a path placeholder doesn't also
// get sent a second time as a query parameter. A placeholder with no matching key in `params` is
// a clear user error (a required part of the URL has nothing to fill it with), so we throw rather
// than silently sending a broken URL with the literal "{name}" text still in it.
static string SubstitutePathParameters(const string &url_template, case_insensitive_map_t<string> &params) {
	string result;
	idx_t pos = 0;
	while (pos < url_template.size()) {
		auto open = url_template.find('{', pos);
		if (open == string::npos) {
			result += url_template.substr(pos);
			break;
		}
		auto close = url_template.find('}', open);
		if (close == string::npos) {
			result += url_template.substr(pos); // an unterminated '{' - not a placeholder, leave as-is
			break;
		}

		result += url_template.substr(pos, open - pos);
		string param_name = url_template.substr(open + 1, close - open - 1);

		auto it = params.find(param_name);
		if (it == params.end()) {
			throw InvalidInputException(
			    "rest_ext: URL \"%s\" requires a value for \"{%s}\" - pass it as a key in the query_params "
			    "argument, e.g. '{\"%s\": \"...\"}'",
			    url_template, param_name, param_name);
		}
		result += it->second;
		params.erase(it);

		pos = close + 1;
	}
	return result;
}

string PerformRestCall(const string &url, const string &method, const string &headers_json,
                       const string &query_params_json, const string &body_json) {
	// Parse the query-parameters JSON into an editable map FIRST, since some of its entries might
	// really be path parameters (see SubstitutePathParameters above) rather than actual query
	// string values - we won't know which is which until we've matched them against the URL.
	case_insensitive_map_t<string> query_params;
	ParseFlatJsonObject(query_params_json,
	                   [&](const string &key, const string &value) { query_params[key] = value; });

	string filled_url = SubstitutePathParameters(url, query_params);

	// Every URL is really two parts for httplib's purposes: the "host" part (scheme + hostname +
	// port) used to open the connection, and the "path" part (everything after the host) used per
	// request. DuckDB core already has a well-tested helper for splitting a URL this way, so we
	// reuse it instead of writing our own URL parser.
	string path;
	string proto_host_port;
	HTTPUtil::DecomposeURL(filled_url, path, proto_host_port);

	// Whatever's left in query_params (i.e. wasn't consumed as a path parameter above) becomes
	// real "?key=value&..." URL-encoded text appended to the path.
	duckdb_httplib_openssl::Params params;
	for (auto &entry : query_params) {
		params.emplace(entry.first, entry.second);
	}
	string path_with_query = params.empty() ? path : duckdb_httplib_openssl::append_query_params(path, params);

	// Turn the headers JSON object into the map type httplib expects for a request's headers.
	//
	// Content-Type gets special treatment: httplib's POST/PUT/PATCH/DELETE calls take it as its
	// own separate argument, not just another header. If we also left "Content-Type" sitting in
	// the headers map, the request would end up with the header listed TWICE - which some servers
	// reject outright. So we pull it out here and use it as the single source of truth below.
	duckdb_httplib_openssl::Headers headers;
	string content_type = "application/json";
	ParseFlatJsonObject(headers_json, [&](const string &key, const string &value) {
		if (StringUtil::CIEquals(key, "Content-Type")) {
			content_type = value;
		} else {
			headers.emplace(key, value);
		}
	});

	duckdb_httplib_openssl::Client client(proto_host_port);
	client.set_connection_timeout(10, 0);
	client.set_read_timeout(30, 0);
	client.set_write_timeout(30, 0);
	client.set_follow_location(true); // follow HTTP redirects automatically

	// A small helper that both turns httplib's result into our plain "return the body" contract,
	// and turns any failure (network error, or a non-2xx HTTP status) into a clear exception.
	auto handle_result = [&](duckdb_httplib_openssl::Result &&res) -> string {
		if (!res) {
			// `res` being "empty" means the request never got a response at all - DNS failure,
			// connection refused, TLS error, timeout, etc.
			throw IOException("rest_ext: HTTP request to \"%s\" failed: %s", filled_url,
			                  duckdb_httplib_openssl::to_string(res.error()));
		}
		if (res->status < 200 || res->status >= 300) {
			throw IOException("rest_ext: HTTP request to \"%s\" returned status %d: %s", filled_url, res->status,
			                  res->body);
		}
		return res->body;
	};

	auto upper_method = StringUtil::Upper(method);
	if (upper_method == "GET") {
		return handle_result(client.Get(path_with_query, headers));
	} else if (upper_method == "POST") {
		return handle_result(client.Post(path_with_query, headers, body_json, content_type));
	} else if (upper_method == "PUT") {
		return handle_result(client.Put(path_with_query, headers, body_json, content_type));
	} else if (upper_method == "PATCH") {
		return handle_result(client.Patch(path_with_query, headers, body_json, content_type));
	} else if (upper_method == "DELETE") {
		return handle_result(client.Delete(path_with_query, headers, body_json, content_type));
	}
	throw InvalidInputException("rest_ext: unsupported HTTP method \"%s\" (expected GET/POST/PUT/PATCH/DELETE)",
	                            method);
}

} // namespace duckdb
