// http_client.hpp
//
// The one function in this extension that actually talks to the network: given a URL, an HTTP
// method, headers, and a body, it makes the real HTTP(S) request and returns the response body as
// plain text. Everything else (parsing JSON, inferring a schema, wiring up ATTACH) builds on top
// of this.
#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Performs one HTTP request and returns the response body.
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
string PerformRestCall(const string &url, const string &method, const string &headers_json,
                       const string &query_params_json, const string &body_json);

} // namespace duckdb
