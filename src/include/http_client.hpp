// http_client.hpp
//
// The one function in this extension that actually talks to the network: given a URL, an HTTP
// method, headers, and a body, it makes the real HTTP(S) request and returns the response body as
// plain text. Everything else (parsing JSON, inferring a schema, wiring up ATTACH) builds on top
// of this.
#pragma once

#include "duckdb.hpp"
#include "rest_call_log.hpp"

namespace duckdb {

// Per-ATTACH settings for how a call is made and recorded; the defaults leave a call exactly as it
// was before they existed (one attempt, httplib's timeouts, no size bounds).
struct RestCallOptions {
	string alias;                 // the ATTACH name, recorded with each call
	idx_t retries = 0;            // RETRIES: extra attempts after a retryable failure
	int64_t retry_backoff_ms = 500; // RETRY_BACKOFF_MS: base of the jittered exponential backoff
	int64_t retry_cap_ms = 30000; // RETRY_CAP_MS: longest single wait, Retry-After included
	int64_t timeout_ms = 0;       // TIMEOUT_MS: connect/read/write timeout; 0 = 10s connect, 30s read/write
	int64_t max_bytes = 0;        // MAX_BYTES: largest response body accepted; 0 = unbounded
	int64_t max_rows = 0;         // MAX_ROWS: rows a scan returns before stopping; 0 = unbounded
	bool rate_limit_headers_set = false;
	vector<string> rate_limit_headers; // RATE_LIMIT_HEADERS: names (a trailing '*' is a prefix) to record
	string user_agent = "rest_ext";
	shared_ptr<RestCallLog> log;  // where calls are recorded; null = not recorded

	bool operator==(const RestCallOptions &other) const {
		return alias == other.alias && retries == other.retries && retry_backoff_ms == other.retry_backoff_ms &&
		       retry_cap_ms == other.retry_cap_ms && timeout_ms == other.timeout_ms && max_bytes == other.max_bytes &&
		       max_rows == other.max_rows && rate_limit_headers_set == other.rate_limit_headers_set &&
		       rate_limit_headers == other.rate_limit_headers && user_agent == other.user_agent;
	}
};

// What a caller adds to one call: the context (for cancelling a backoff wait), which page of a
// paged read it is, and the endpoint label to record (default: derived from the URL passed in).
struct RestCallTrace {
	ClientContext *context = nullptr;
	idx_t page_index = 0;
	string endpoint;
};

struct RestResponse {
	string body;
	int status = 0;
	string url; // the URL the response came from, after any same-origin redirect
	string link_header; // every Link response header, joined with ", "
	uint64_t call_id = 0; // the call's id in the RestCallLog (0 = not recorded)
};

// "host/path" of a URL with the scheme, credentials, query string and fragment removed. Passed a
// template (https://h/v1/things/{id}) it keeps the placeholder, never a value.
string RestEndpointLabel(const string &url);

// Performs one HTTP request (with the retries and bounds in `options`) and returns the response.
//
//   url               - the URL to request, e.g. "https://api.example.com/v1/things". May contain
//                       {name} placeholders (e.g. "https://api.example.com/v1/things/{id}") - any
//                       such placeholder is filled in from query_params_json's matching key
//                       BEFORE that key is considered for the actual query string, so a caller
//                       passing '{"id": "42"}' against that URL both fills in the path and does
//                       NOT also send "?id=42". A placeholder with no matching key throws a clear
//                       error instead of silently sending a broken URL.
//   method            - "GET", "POST", "PUT", "PATCH", or "DELETE" (case-insensitive)
//   headers_json      - a flat JSON object of request headers, e.g. '{"Accept":"application/json"}'
//   query_params_json - a flat JSON object; each key not consumed as a path placeholder (see
//                       above) is appended to the URL as "?key=value&...", e.g. '{"q":"x"}'
//   body_json         - the request body sent for POST/PUT/PATCH/DELETE (ignored for GET)
//
// Throws IOException if the connection fails outright, or if the server responds with a status
// code outside the 200-299 "success" range.
RestResponse PerformRestCall(const string &url, const string &method, const string &headers_json,
                             const string &query_params_json, const string &body_json,
                             const RestCallOptions &options, const RestCallTrace &trace);

// PerformRestCall for a URL that is already complete - e.g. a next-page link an API returned - so
// no {placeholder} is filled in and no query parameter is appended.
RestResponse PerformRestCallToUrl(const string &url, const string &method, const string &headers_json,
                                  const string &body_json, const RestCallOptions &options,
                                  const RestCallTrace &trace);

// The "scheme://host[:port]" a URL points at, lower-cased, for same-origin checks.
string RestUrlOrigin(const string &url);

// The target of the `rel="next"` entry in a Link header, resolved against `request_url`; empty if
// there is none.
string RestNextLinkTarget(const string &link_header, const string &request_url);

} // namespace duckdb
