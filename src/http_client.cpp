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
// A placeholder value is data, not URL syntax. In the path it is percent-encoded (keeping '/', so a
// value can span segments) and may not contain a ".." segment; in the host it may only hold
// letters, digits, '-' and '.'. Otherwise a value such as "x?admin=1#" or "evil.example/" could
// rewrite the request - or send its headers to a different host.
static string EncodePlaceholderValue(const string &name, const string &value, bool in_authority) {
	string encoded;
	for (auto c : value) {
		auto byte = static_cast<unsigned char>(c);
		bool unreserved = StringUtil::CharacterIsAlphaNumeric(c) || c == '-' || c == '.' || c == '_' || c == '~';
		if (in_authority) {
			if (!StringUtil::CharacterIsAlphaNumeric(c) && c != '-' && c != '.') {
				throw InvalidInputException("rest_ext: the value for \"{%s}\" is part of the host name, so it may "
				                            "only contain letters, digits, '-' and '.'",
				                            name);
			}
			encoded += c;
		} else if (unreserved || c == '/') {
			encoded += c;
		} else {
			static const char *hex = "0123456789ABCDEF";
			encoded += '%';
			encoded += hex[byte >> 4];
			encoded += hex[byte & 0xF];
		}
	}
	if (!in_authority) {
		auto segments = StringUtil::Split(value, '/');
		for (auto &segment : segments) {
			if (segment == "..") {
				throw InvalidInputException("rest_ext: the value for \"{%s}\" may not contain a \"..\" path segment",
				                            name);
			}
		}
	}
	return encoded;
}

static string SubstitutePathParameters(const string &url_template, case_insensitive_map_t<string> &params) {
	string result;
	idx_t pos = 0;
	auto scheme_end = url_template.find("://");
	auto authority_start = scheme_end == string::npos ? 0 : scheme_end + 3;
	auto authority_end = url_template.find_first_of("/?#", authority_start);
	if (authority_end == string::npos) {
		authority_end = url_template.size();
	}
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
		result += EncodePlaceholderValue(param_name, it->second, open < authority_end);
		params.erase(it);

		pos = close + 1;
	}
	return result;
}

// httplib writes request headers verbatim, so a CR or LF in a name or value would end the header
// early and let the rest be read as further headers (or a request body). Refuse any control
// character instead; a name must also be a non-empty RFC 9110 token.
static void RequireSafeHeader(const string &key, const string &value) {
	static const string token_symbols = "!#$%&'*+-.^_`|~";
	bool valid_name = !key.empty();
	for (auto c : key) {
		if (!StringUtil::CharacterIsAlphaNumeric(c) && token_symbols.find(c) == string::npos) {
			valid_name = false;
		}
	}
	if (!valid_name) {
		throw InvalidInputException("rest_ext: \"%s\" is not a valid HTTP header name", key);
	}
	for (auto c : value) {
		auto byte = static_cast<unsigned char>(c);
		if ((byte < 0x20 && c != '\t') || byte == 0x7F) {
			throw InvalidInputException("rest_ext: the value of HTTP header \"%s\" contains a control character",
			                            key);
		}
	}
}

static string ExecuteRestRequest(const string &filled_url, const case_insensitive_map_t<string> &query_params,
                                 const string &method, const string &headers_json, const string &body_json);

string PerformRestCall(const string &url, const string &method, const string &headers_json,
                       const string &query_params_json, const string &body_json) {
	// Parse the query-parameters JSON into an editable map FIRST, since some of its entries might
	// really be path parameters (see SubstitutePathParameters above) rather than actual query
	// string values - we won't know which is which until we've matched them against the URL.
	case_insensitive_map_t<string> query_params;
	ParseFlatJsonObject(query_params_json,
	                   [&](const string &key, const string &value) { query_params[key] = value; });

	string filled_url = SubstitutePathParameters(url, query_params);
	return ExecuteRestRequest(filled_url, query_params, method, headers_json, body_json);
}

string PerformRestCallToUrl(const string &url, const string &method, const string &headers_json,
                            const string &body_json) {
	case_insensitive_map_t<string> no_params;
	return ExecuteRestRequest(url, no_params, method, headers_json, body_json);
}

string RestUrlOrigin(const string &url) {
	string path;
	string proto_host_port;
	HTTPUtil::DecomposeURL(url, path, proto_host_port);
	return StringUtil::Lower(proto_host_port);
}

static string ExecuteRestRequest(const string &filled_url, const case_insensitive_map_t<string> &query_params,
                                 const string &method, const string &headers_json, const string &body_json) {
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
		RequireSafeHeader(key, value);
		if (StringUtil::CIEquals(key, "Content-Type")) {
			content_type = value;
		} else {
			headers.emplace(key, value);
		}
	});

	auto upper_method = StringUtil::Upper(method);
	if (upper_method != "GET" && upper_method != "POST" && upper_method != "PUT" && upper_method != "PATCH" &&
	    upper_method != "DELETE") {
		throw InvalidInputException("rest_ext: unsupported HTTP method \"%s\" (expected GET/POST/PUT/PATCH/DELETE)",
		                            method);
	}

	// Redirects are followed by hand, and only within the origin the request was sent to: httplib's
	// own follower drops Authorization on a cross-host hop but forwards every other header, which
	// would hand an API key in e.g. X-Api-Key to whatever host the server names.
	static constexpr idx_t MAX_REDIRECTS = 5;
	auto origin = StringUtil::Lower(proto_host_port);
	string current_url = filled_url;
	for (idx_t hop = 0; hop <= MAX_REDIRECTS; hop++) {
		duckdb_httplib_openssl::Client client(proto_host_port);
		client.set_connection_timeout(10, 0);
		client.set_read_timeout(30, 0);
		client.set_write_timeout(30, 0);
		client.set_follow_location(false);

		duckdb_httplib_openssl::Result res = [&]() {
			if (upper_method == "GET") {
				return client.Get(path_with_query, headers);
			} else if (upper_method == "POST") {
				return client.Post(path_with_query, headers, body_json, content_type);
			} else if (upper_method == "PUT") {
				return client.Put(path_with_query, headers, body_json, content_type);
			} else if (upper_method == "PATCH") {
				return client.Patch(path_with_query, headers, body_json, content_type);
			}
			return client.Delete(path_with_query, headers, body_json, content_type);
		}();
		if (!res) {
			// `res` being "empty" means the request never got a response at all - DNS failure,
			// connection refused, TLS error, timeout, etc.
			throw IOException("rest_ext: HTTP request to \"%s\" failed: %s", current_url,
			                  duckdb_httplib_openssl::to_string(res.error()));
		}
		if (res->status >= 300 && res->status < 400 && res->has_header("Location")) {
			auto location = res->get_header_value("Location");
			if (!location.empty() && location[0] == '/') {
				current_url = proto_host_port + location;
			} else {
				current_url = location;
			}
			if (RestUrlOrigin(current_url) != origin) {
				throw IOException("rest_ext: HTTP request to \"%s\" was redirected to another origin (\"%s\"); "
				                  "not following it, so the request's headers are not sent there",
				                  filled_url, current_url);
			}
			HTTPUtil::DecomposeURL(current_url, path_with_query, proto_host_port);
			continue;
		}
		if (res->status < 200 || res->status >= 300) {
			throw IOException("rest_ext: HTTP request to \"%s\" returned status %d: %s", current_url, res->status,
			                  res->body);
		}
		return res->body;
	}
	throw IOException("rest_ext: HTTP request to \"%s\" was redirected more than %d times", filled_url,
	                  MAX_REDIRECTS);
}

} // namespace duckdb
