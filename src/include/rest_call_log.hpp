// rest_call_log.hpp
//
// An in-memory ring of the most recent HTTP calls rest_ext made in one DuckDB instance, exposed as
// the table function rest_ext_calls() and cleared by rest_ext_calls_clear(). A record holds the
// endpoint as host + path template and the rate-limit headers asked for; never a header value that
// was sent, a query string, or a path-parameter value.
#pragma once

#include "duckdb.hpp"
#include "rest_ext_compat.hpp"

#ifdef REST_EXT_HAS_EXTENSION_LOADER
#include "duckdb/main/extension/extension_loader.hpp"
#else
#include "duckdb/main/extension_util.hpp"
#endif

#include <deque>
#include <mutex>

namespace duckdb {

struct RestCallRecord {
	string alias;
	string endpoint;
	string method;
	int32_t page_index = 0;
	timestamp_t started_at;
	int64_t latency_ms = 0;
	int32_t status = -1; // -1 = no response (NULL)
	string outcome = "ok";
	int64_t bytes = 0;
	int64_t rows = -1; // -1 = not known (NULL)
	int32_t retries = 0;
	int64_t waited_ms = 0;
	int64_t retry_after_ms = -1; // -1 = no Retry-After (NULL)
	string rate_limit;           // JSON object, empty = none (NULL)
	string error;                // empty = none (NULL)
};

class RestCallLog {
public:
	static constexpr idx_t CAPACITY = 500;

	// Appends a record and returns its id, valid for SetRows/SetOutcome until it falls out of the ring.
	uint64_t Add(RestCallRecord record);
	void SetRows(uint64_t id, int64_t rows);
	void SetOutcome(uint64_t id, const string &outcome, const string &error);
	vector<RestCallRecord> Snapshot();
	idx_t Clear();

private:
	std::mutex lock;
	std::deque<RestCallRecord> records;
	uint64_t first_id = 1; // id of records.front()
};

// Creates the log for a DuckDB instance and registers rest_ext_calls() and rest_ext_calls_clear()
// on it. Called once per instance when the extension loads.
#ifdef REST_EXT_HAS_EXTENSION_LOADER
void RegisterRestCallLog(ExtensionLoader &loader);
#else
void RegisterRestCallLog(DatabaseInstance &db);
#endif

// The log of the instance a ClientContext belongs to; null if the extension was not loaded there.
shared_ptr<RestCallLog> FindRestCallLog(ClientContext &context);

} // namespace duckdb
