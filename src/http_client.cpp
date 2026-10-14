#include "http_client.hpp"

#include "json_helpers.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"

// This file builds its own OpenSSL-enabled instantiation of the vendored, header-only httplib
// library (it lives at duckdb/third_party/httplib, bundled with DuckDB itself - not something we
// added). Defining CPPHTTPLIB_OPENSSL_SUPPORT before including it is what turns on HTTPS support;
// DuckDB core's own (separate) use of this same header does NOT define this macro, so linking
// OpenSSL in CMakeLists.txt alone would not be enough without this #define here too.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <chrono>
#include <cmath>
#include <random>
#include <thread>

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

static RestResponse ExecuteRestRequest(const string &filled_url, const case_insensitive_map_t<string> &query_params,
                                       const string &method, const string &headers_json, const string &body_json,
                                       const RestCallOptions &options, const RestCallTrace &trace,
                                       const string &endpoint);

string RestEndpointLabel(const string &url) {
	auto start = url.find("://");
	start = start == string::npos ? 0 : start + 3;
	auto end = url.find_first_of("?#", start);
	auto label = url.substr(start, end == string::npos ? string::npos : end - start);
	auto authority_end = label.find('/');
	auto at = label.find('@');
	if (at != string::npos && (authority_end == string::npos || at < authority_end)) {
		label = label.substr(at + 1);
	}
	return label;
}

RestResponse PerformRestCall(const string &url, const string &method, const string &headers_json,
                             const string &query_params_json, const string &body_json,
                             const RestCallOptions &options, const RestCallTrace &trace) {
	// Parse the query-parameters JSON into an editable map FIRST, since some of its entries might
	// really be path parameters (see SubstitutePathParameters above) rather than actual query
	// string values - we won't know which is which until we've matched them against the URL.
	case_insensitive_map_t<string> query_params;
	ParseFlatJsonObject(query_params_json,
	                   [&](const string &key, const string &value) { query_params[key] = value; });

	string filled_url = SubstitutePathParameters(url, query_params);
	auto endpoint = trace.endpoint.empty() ? RestEndpointLabel(url) : trace.endpoint;
	return ExecuteRestRequest(filled_url, query_params, method, headers_json, body_json, options, trace, endpoint);
}

RestResponse PerformRestCallToUrl(const string &url, const string &method, const string &headers_json,
                                  const string &body_json, const RestCallOptions &options,
                                  const RestCallTrace &trace) {
	case_insensitive_map_t<string> no_params;
	auto endpoint = trace.endpoint.empty() ? RestEndpointLabel(url) : trace.endpoint;
	return ExecuteRestRequest(url, no_params, method, headers_json, body_json, options, trace, endpoint);
}

string RestUrlOrigin(const string &url) {
	string path;
	string proto_host_port;
	HTTPUtil::DecomposeURL(url, path, proto_host_port);
	return StringUtil::Lower(proto_host_port);
}

// Splits a Link header into its entries and returns the target of the one whose rel includes
// "next". Targets are resolved against the URL the header came from.
string RestNextLinkTarget(const string &link_header, const string &request_url) {
	idx_t pos = 0;
	while (pos < link_header.size()) {
		auto open = link_header.find('<', pos);
		if (open == string::npos) {
			return string();
		}
		auto close = link_header.find('>', open);
		if (close == string::npos) {
			return string();
		}
		auto target = link_header.substr(open + 1, close - open - 1);
		// The parameters run to the next ',' that is outside a quoted string.
		idx_t end = close + 1;
		bool quoted = false;
		while (end < link_header.size() && (quoted || link_header[end] != ',')) {
			if (link_header[end] == '"') {
				quoted = !quoted;
			}
			end++;
		}
		auto params = link_header.substr(close + 1, end - close - 1);
		pos = end + 1;

		bool is_next = false;
		for (auto &param : StringUtil::Split(params, ';')) {
			auto eq = param.find('=');
			if (eq == string::npos) {
				continue;
			}
			auto name = param.substr(0, eq);
			auto value = param.substr(eq + 1);
			StringUtil::Trim(name);
			StringUtil::Trim(value);
			if (!StringUtil::CIEquals(name, "rel")) {
				continue;
			}
			if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
				value = value.substr(1, value.size() - 2);
			}
			for (auto &rel : StringUtil::Split(value, ' ')) {
				if (StringUtil::CIEquals(rel, "next")) {
					is_next = true;
				}
			}
		}
		if (!is_next || target.empty()) {
			continue;
		}
		if (target.find("://") != string::npos) {
			return target;
		}
		string request_path;
		string request_origin;
		HTTPUtil::DecomposeURL(request_url, request_path, request_origin);
		if (target[0] == '/') {
			return request_origin + target;
		}
		auto query = request_path.find('?');
		auto base_path = request_path.substr(0, query);
		auto slash = base_path.rfind('/');
		return request_origin + (slash == string::npos ? string("/") : base_path.substr(0, slash + 1)) + target;
	}
	return string();
}

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm).
static int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
	y -= m <= 2;
	auto era = (y >= 0 ? y : y - 399) / 400;
	auto yoe = y - era * 400;
	auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

// Retry-After is delta-seconds or an HTTP date (IMF-fixdate, "Sun, 06 Nov 1994 08:49:37 GMT").
// Returns the wait in milliseconds, or -1 if the value is neither.
static int64_t ParseRetryAfterMs(const string &value_p) {
	auto value = value_p;
	StringUtil::Trim(value);
	if (value.empty()) {
		return -1;
	}
	if (StringUtil::CharacterIsDigit(value[0])) {
		char *end = nullptr;
		double seconds = std::strtod(value.c_str(), &end);
		if (end && *end == '\0' && std::isfinite(seconds) && seconds >= 0 && seconds < 1e9) {
			return static_cast<int64_t>(seconds * 1000.0);
		}
		return -1;
	}
	static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	auto comma = value.find(',');
	char month_name[4] = {0};
	int day = 0, year = 0, hour = 0, minute = 0, second = 0;
	if (comma == string::npos || std::sscanf(value.c_str() + comma + 1, " %d %3s %d %d:%d:%d", &day, month_name, &year,
	                                         &hour, &minute, &second) != 6) {
		return -1;
	}
	int month = 0;
	for (int i = 0; i < 12; i++) {
		if (StringUtil::CIEquals(month_name, months[i])) {
			month = i + 1;
		}
	}
	if (month == 0) {
		return -1;
	}
	int64_t target_ms = (DaysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second) * 1000;
	int64_t now_ms = Timestamp::GetEpochMs(Timestamp::GetCurrentTimestamp());
	return MaxValue<int64_t>(0, target_ms - now_ms);
}

// Exponential backoff with jitter: the wait is uniform in [d/2, d] where d = min(cap, base * 2^attempt).
static int64_t BackoffDelayMs(idx_t attempt, int64_t base_ms, int64_t cap_ms) {
	thread_local std::mt19937_64 generator {std::random_device {}()};
	double scaled = static_cast<double>(base_ms) * std::pow(2.0, static_cast<double>(MinValue<idx_t>(attempt, 30)));
	auto ceiling = static_cast<int64_t>(MinValue<double>(scaled, static_cast<double>(cap_ms)));
	std::uniform_int_distribution<int64_t> jitter(ceiling / 2, ceiling);
	return jitter(generator);
}

static string LowerCopy(const string &text) {
	return StringUtil::Lower(text);
}

static bool NameMatches(const string &pattern, const string &name) {
	if (!pattern.empty() && pattern.back() == '*') {
		return StringUtil::StartsWith(name, pattern.substr(0, pattern.size() - 1));
	}
	return pattern == name;
}

// The response headers to record: the configured names, or by default Retry-After and the common
// rate-limit families. Repeated headers are joined; values are cut to 256 characters.
static string CollectRateLimit(const duckdb_httplib_openssl::Headers &headers, const RestCallOptions &options) {
	static const vector<string> defaults = {"retry-after", "x-ratelimit-*", "ratelimit-*", "x-ms-ratelimit-remaining-*"};
	vector<string> patterns;
	if (options.rate_limit_headers_set) {
		for (auto &name : options.rate_limit_headers) {
			patterns.push_back(LowerCopy(name));
		}
	} else {
		patterns = defaults;
	}
	case_insensitive_map_t<string> found;
	for (auto &header : headers) {
		auto name = LowerCopy(header.first);
		bool wanted = false;
		for (auto &pattern : patterns) {
			wanted = wanted || NameMatches(pattern, name);
		}
		if (!wanted) {
			continue;
		}
		auto value = header.second.substr(0, 256);
		auto existing = found.find(name);
		if (existing == found.end()) {
			found[name] = value;
		} else {
			existing->second += ", " + value;
		}
	}
	return found.empty() ? string() : BuildFlatJsonObject(found);
}

// Cuts a response body down to a short single-line excerpt for the error column, with the values
// the request itself carried (header values, query parameter values) masked in case the API echoes
// them back.
static string ScrubExcerpt(const string &body, const vector<string> &secrets) {
	auto excerpt = body.substr(0, 200);
	for (auto &c : excerpt) {
		if (static_cast<unsigned char>(c) < 0x20) {
			c = ' ';
		}
	}
	for (auto &secret : secrets) {
		if (secret.size() >= 4) {
			excerpt = StringUtil::Replace(excerpt, secret, "***");
		}
	}
	return excerpt;
}

static void SleepWithInterrupt(int64_t total_ms, ClientContext *context) {
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		if (context && context->interrupted) {
			throw InterruptException();
		}
		auto remaining =
		    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(50)));
	}
}

static bool IsRetryableStatus(int status) {
	return status == 429 || status == 502 || status == 503 || status == 504;
}

static bool IsRetryableNetworkError(duckdb_httplib_openssl::Error error) {
	using duckdb_httplib_openssl::Error;
	return error == Error::Connection || error == Error::Read || error == Error::Write ||
	       error == Error::ConnectionTimeout;
}

// One HTTP exchange: either a response (status, headers, body) or an error.
struct RestAttempt {
	bool has_response = false;
	int status = 0;
	duckdb_httplib_openssl::Headers headers;
	string body;
	duckdb_httplib_openssl::Error error = duckdb_httplib_openssl::Error::Success;
	bool bound_hit = false; // the body passed MAX_BYTES and was cut off
	bool timed_out = false;
	int64_t elapsed_ms = 0;
};

static RestAttempt SendOnce(const string &proto_host_port, const string &upper_method, const string &path_with_query,
                            const duckdb_httplib_openssl::Headers &headers, const string &body_json,
                            const string &content_type, const RestCallOptions &options) {
	using namespace duckdb_httplib_openssl;
	RestAttempt attempt;
	auto started = std::chrono::steady_clock::now();

	Client client(proto_host_port);
	time_t connect_sec = 10, connect_usec = 0, io_sec = 30, io_usec = 0;
	if (options.timeout_ms > 0) {
		connect_sec = io_sec = static_cast<time_t>(options.timeout_ms / 1000);
		connect_usec = io_usec = static_cast<time_t>((options.timeout_ms % 1000) * 1000);
	}
	client.set_connection_timeout(connect_sec, connect_usec);
	client.set_read_timeout(io_sec, io_usec);
	client.set_write_timeout(io_sec, io_usec);
	client.set_follow_location(false);

	Request request;
	request.method = upper_method;
	request.path = path_with_query;
	request.headers = headers;
	if (upper_method != "GET") {
		request.body = body_json;
		request.headers.emplace("Content-Type", content_type);
	}
	request.response_handler = [&](const Response &response) {
		attempt.has_response = true;
		attempt.status = response.status;
		attempt.headers = response.headers;
		if (options.max_bytes > 0 && response.has_header("Content-Length") &&
		    std::strtoull(response.get_header_value("Content-Length").c_str(), nullptr, 10) >
		        static_cast<unsigned long long>(options.max_bytes)) {
			attempt.bound_hit = true;
			return false;
		}
		return true;
	};
	request.content_receiver = [&](const char *data, size_t length, uint64_t, uint64_t) {
		attempt.body.append(data, length);
		if (options.max_bytes > 0 && attempt.body.size() > static_cast<size_t>(options.max_bytes)) {
			attempt.bound_hit = true;
			return false;
		}
		return true;
	};

	auto result = client.send(request);
	attempt.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
	                         .count();
	if (!result) {
		// A failure after the status line (e.g. a reset mid-body) is a failed call, not a response; a body
		// cut off at MAX_BYTES keeps its status.
		attempt.has_response = attempt.has_response && attempt.bound_hit;
		attempt.error = result.error();
		// httplib reports a stalled read as a plain read error; one that ran out the clock is a timeout.
		auto io_ms = io_sec * 1000 + io_usec / 1000;
		attempt.timed_out = attempt.error == Error::ConnectionTimeout ||
		                    ((attempt.error == Error::Read || attempt.error == Error::Write) &&
		                     attempt.elapsed_ms >= static_cast<int64_t>(io_ms) * 9 / 10);
	}
	return attempt;
}

// Everything but the redirect-following loop's bookkeeping: builds the request, then sends it with
// retries and records the call. `rec` is filled in as the call goes and logged however it ends.
static RestResponse RunRestRequest(const string &filled_url, const duckdb_httplib_openssl::Params &params,
                                   const string &upper_method, const duckdb_httplib_openssl::Headers &headers,
                                   const string &content_type, const string &body_json, const vector<string> &secrets,
                                   const RestCallOptions &options, const RestCallTrace &trace, RestCallRecord &rec) {
	using duckdb_httplib_openssl::Error;
	string path;
	string proto_host_port;
	HTTPUtil::DecomposeURL(filled_url, path, proto_host_port);
	string path_with_query = params.empty() ? path : duckdb_httplib_openssl::append_query_params(path, params);

	// Redirects are followed by hand, and only within the origin the request was sent to: httplib's
	// own follower drops Authorization on a cross-host hop but forwards every other header, which
	// would hand an API key in e.g. X-Api-Key to whatever host the server names.
	static constexpr idx_t MAX_REDIRECTS = 5;
	auto origin = StringUtil::Lower(proto_host_port);
	string current_url = filled_url;
	for (idx_t hop = 0; hop <= MAX_REDIRECTS; hop++) {
		RestAttempt attempt;
		int64_t attempt_retry_after = -1; // from the latest response
		for (idx_t tries = 0;; tries++) {
			attempt_retry_after = -1;
			rec.status = -1;
			rec.rate_limit.clear();
			attempt = SendOnce(proto_host_port, upper_method, path_with_query, headers, body_json, content_type, options);
			rec.bytes = NumericCast<int64_t>(attempt.body.size());
			if (attempt.has_response) {
				rec.status = attempt.status;
				rec.rate_limit = CollectRateLimit(attempt.headers, options);
				auto header = attempt.headers.find("Retry-After");
				attempt_retry_after = header == attempt.headers.end() ? -1 : ParseRetryAfterMs(header->second);
				if (attempt_retry_after >= 0) {
					rec.retry_after_ms = attempt_retry_after;
				}
			}
			bool retryable = !attempt.bound_hit && (attempt.has_response ? IsRetryableStatus(attempt.status)
			                                                             : IsRetryableNetworkError(attempt.error));
			if (!retryable || tries >= options.retries) {
				break;
			}
			int64_t delay = attempt_retry_after >= 0
			                    ? MinValue<int64_t>(attempt_retry_after, options.retry_cap_ms)
			                    : BackoffDelayMs(tries, options.retry_backoff_ms, options.retry_cap_ms);
			rec.retries++;
			rec.waited_ms += delay;
			rec.error = "interrupted";
			SleepWithInterrupt(delay, trace.context);
			rec.error.clear();
		}

		auto retried = rec.retries > 0 ? StringUtil::Format(" (after %d retries)", rec.retries) : string();
		if (attempt.bound_hit) {
			rec.outcome = "bound_hit";
			rec.error = "response is larger than MAX_BYTES (" + std::to_string(options.max_bytes) + ")";
			throw IOException("rest_ext: the response from \"%s\" is larger than MAX_BYTES (%lld); not reading it",
			                  current_url, static_cast<long long>(options.max_bytes));
		}
		if (!attempt.has_response) {
			// The request never got a response at all - DNS failure, connection refused, TLS error,
			// timeout, etc.
			auto reason = duckdb_httplib_openssl::to_string(attempt.error);
			rec.outcome = attempt.timed_out ? "timeout" : "network";
			rec.error = attempt.timed_out ? "timed out: " + reason : reason;
			throw IOException("rest_ext: HTTP request to \"%s\" failed: %s%s", current_url, reason, retried);
		}
		if (attempt.status >= 300 && attempt.status < 400 && attempt.headers.find("Location") != attempt.headers.end()) {
			auto location = attempt.headers.find("Location")->second;
			if (!location.empty() && location[0] == '/') {
				current_url = proto_host_port + location;
			} else {
				current_url = location;
			}
			if (RestUrlOrigin(current_url) != origin) {
				rec.outcome = "http_error";
				rec.error = "redirected to another origin; not followed";
				throw IOException("rest_ext: HTTP request to \"%s\" was redirected to another origin (\"%s\"); "
				                  "not following it, so the request's headers are not sent there",
				                  filled_url, current_url);
			}
			HTTPUtil::DecomposeURL(current_url, path_with_query, proto_host_port);
			continue;
		}
		if (attempt.status < 200 || attempt.status >= 300) {
			bool limited = attempt.status == 429 || (attempt.status == 503 && attempt_retry_after >= 0);
			rec.outcome = limited ? "rate_limited" : "http_error";
			rec.error = "HTTP " + std::to_string(attempt.status) + ": " + ScrubExcerpt(attempt.body, secrets);
			throw IOException("rest_ext: HTTP request to \"%s\" returned status %d%s: %s", current_url, attempt.status,
			                  retried, attempt.body);
		}

		RestResponse response;
		response.body = std::move(attempt.body);
		response.status = attempt.status;
		response.url = current_url;
		auto links = attempt.headers.equal_range("Link");
		for (auto it = links.first; it != links.second; ++it) {
			response.link_header += (response.link_header.empty() ? "" : ", ") + it->second;
		}
		rec.outcome = "ok";
		return response;
	}
	rec.outcome = "http_error";
	rec.error = "too many redirects";
	throw IOException("rest_ext: HTTP request to \"%s\" was redirected more than %d times", filled_url,
	                  MAX_REDIRECTS);
}

static RestResponse ExecuteRestRequest(const string &filled_url, const case_insensitive_map_t<string> &query_params,
                                       const string &method, const string &headers_json, const string &body_json,
                                       const RestCallOptions &options, const RestCallTrace &trace,
                                       const string &endpoint) {
	// Whatever's left in query_params (i.e. wasn't consumed as a path parameter above) becomes
	// real "?key=value&..." URL-encoded text appended to the path.
	duckdb_httplib_openssl::Params params;
	vector<string> secrets;
	for (auto &entry : query_params) {
		params.emplace(entry.first, entry.second);
		secrets.push_back(entry.second);
	}

	// Turn the headers JSON object into the map type httplib expects for a request's headers.
	//
	// Content-Type gets special treatment: httplib's POST/PUT/PATCH/DELETE calls take it as its
	// own separate argument, not just another header. If we also left "Content-Type" sitting in
	// the headers map, the request would end up with the header listed TWICE - which some servers
	// reject outright. So we pull it out here and use it as the single source of truth below.
	duckdb_httplib_openssl::Headers headers;
	string content_type = "application/json";
	bool has_user_agent = false;
	ParseFlatJsonObject(headers_json, [&](const string &key, const string &value) {
		RequireSafeHeader(key, value);
		if (StringUtil::CIEquals(key, "Content-Type")) {
			content_type = value;
		} else {
			headers.emplace(key, value);
			if (StringUtil::CIEquals(key, "User-Agent")) {
				has_user_agent = true;
			} else if (!StringUtil::CIEquals(key, "Accept") && !StringUtil::CIEquals(key, "Accept-Encoding")) {
				secrets.push_back(value);
				auto space = value.find(' ');
				if (space != string::npos) {
					secrets.push_back(value.substr(space + 1));
				}
			}
		}
	});
	if (!has_user_agent) {
		headers.emplace("User-Agent", options.user_agent);
	}

	auto upper_method = StringUtil::Upper(method);
	if (upper_method != "GET" && upper_method != "POST" && upper_method != "PUT" && upper_method != "PATCH" &&
	    upper_method != "DELETE") {
		throw InvalidInputException("rest_ext: unsupported HTTP method \"%s\" (expected GET/POST/PUT/PATCH/DELETE)",
		                            method);
	}

	auto url_query = filled_url.find('?');
	if (url_query != string::npos) {
		for (auto &pair : StringUtil::Split(filled_url.substr(url_query + 1), '&')) {
			auto eq = pair.find('=');
			if (eq != string::npos) {
				secrets.push_back(pair.substr(eq + 1));
			}
		}
	}

	RestCallRecord rec;
	rec.alias = options.alias;
	rec.endpoint = endpoint;
	rec.method = upper_method;
	rec.page_index = NumericCast<int32_t>(trace.page_index);
	rec.started_at = Timestamp::GetCurrentTimestamp();
	rec.outcome = "network";
	auto started = std::chrono::steady_clock::now();
	auto finish = [&]() {
		rec.latency_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		return options.log ? options.log->Add(rec) : 0;
	};

	RestResponse response;
	try {
		response = RunRestRequest(filled_url, params, upper_method, headers, content_type, body_json, secrets, options,
		                          trace, rec);
	} catch (...) {
		finish();
		throw;
	}
	response.call_id = finish();
	return response;
}

} // namespace duckdb
