#include "rest_call_log.hpp"

#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <unordered_map>

namespace duckdb {

uint64_t RestCallLog::Add(RestCallRecord record) {
	std::lock_guard<std::mutex> guard(lock);
	records.push_back(std::move(record));
	if (records.size() > CAPACITY) {
		records.pop_front();
		first_id++;
	}
	return first_id + records.size() - 1;
}

void RestCallLog::SetRows(uint64_t id, int64_t rows) {
	std::lock_guard<std::mutex> guard(lock);
	if (id >= first_id && id < first_id + records.size()) {
		records[id - first_id].rows = rows;
	}
}

void RestCallLog::SetOutcome(uint64_t id, const string &outcome, const string &error) {
	std::lock_guard<std::mutex> guard(lock);
	if (id >= first_id && id < first_id + records.size()) {
		auto &record = records[id - first_id];
		record.outcome = outcome;
		if (!error.empty()) {
			record.error = error;
		}
	}
}

vector<RestCallRecord> RestCallLog::Snapshot() {
	std::lock_guard<std::mutex> guard(lock);
	return vector<RestCallRecord>(records.begin(), records.end());
}

idx_t RestCallLog::Clear() {
	std::lock_guard<std::mutex> guard(lock);
	auto count = records.size();
	first_id += count;
	records.clear();
	return count;
}

// One log per DuckDB instance. The instance owns it through the table functions it registered; the
// registry only lets ATTACH find it again, so it holds a weak pointer.
static std::mutex registry_lock;
static std::unordered_map<const DatabaseInstance *, weak_ptr<RestCallLog>> registry;

shared_ptr<RestCallLog> FindRestCallLog(ClientContext &context) {
	std::lock_guard<std::mutex> guard(registry_lock);
	auto entry = registry.find(context.db.get());
	return entry == registry.end() ? nullptr : entry->second.lock();
}

struct RestCallLogInfo : public TableFunctionInfo {
	shared_ptr<RestCallLog> log;
};

static Value NullableText(const string &text) {
	return text.empty() ? Value(LogicalType::VARCHAR) : Value(text);
}

static Value NullableBigint(int64_t value) {
	return value < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(value);
}

struct RestCallsBindData : public FunctionData {
	shared_ptr<RestCallLog> log;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<RestCallsBindData>();
		copy->log = log;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other) const override {
		return log == other.Cast<RestCallsBindData>().log;
	}
};

static unique_ptr<RestCallsBindData> BindToLog(TableFunctionBindInput &input) {
	auto bind = make_uniq<RestCallsBindData>();
	bind->log = input.info->Cast<RestCallLogInfo>().log;
	return bind;
}

struct RestCallsGlobalState : public GlobalTableFunctionState {
	vector<RestCallRecord> records;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> RestCallsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	names = {"alias",      "endpoint",   "method",         "page_index", "started_at", "latency_ms", "status",
	         "outcome",    "bytes",      "rows",           "retries",    "waited_ms",  "retry_after_ms",
	         "rate_limit", "error"};
	return_types = {LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::TIMESTAMP, LogicalType::BIGINT,  LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::BIGINT,    LogicalType::BIGINT,  LogicalType::INTEGER, LogicalType::BIGINT,
	                LogicalType::BIGINT,    LogicalType::VARCHAR, LogicalType::VARCHAR};
	return BindToLog(input);
}

static unique_ptr<GlobalTableFunctionState> RestCallsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<RestCallsGlobalState>();
	state->records = input.bind_data->Cast<RestCallsBindData>().log->Snapshot();
	return std::move(state);
}

static void RestCallsScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RestCallsGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.records.size()) {
		auto &r = state.records[state.offset++];
		output.SetValue(0, count, Value(r.alias));
		output.SetValue(1, count, Value(r.endpoint));
		output.SetValue(2, count, Value(r.method));
		output.SetValue(3, count, Value::INTEGER(r.page_index));
		output.SetValue(4, count, Value::TIMESTAMP(r.started_at));
		output.SetValue(5, count, Value::BIGINT(r.latency_ms));
		output.SetValue(6, count, r.status < 0 ? Value(LogicalType::INTEGER) : Value::INTEGER(r.status));
		output.SetValue(7, count, Value(r.outcome));
		output.SetValue(8, count, Value::BIGINT(r.bytes));
		output.SetValue(9, count, NullableBigint(r.rows));
		output.SetValue(10, count, Value::INTEGER(r.retries));
		output.SetValue(11, count, Value::BIGINT(r.waited_ms));
		output.SetValue(12, count, NullableBigint(r.retry_after_ms));
		output.SetValue(13, count, NullableText(r.rate_limit));
		output.SetValue(14, count, NullableText(r.error));
		count++;
	}
	output.SetCardinality(count);
}

struct RestCallsClearGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> RestCallsClearBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	names = {"cleared"};
	return_types = {LogicalType::BIGINT};
	return BindToLog(input);
}

static unique_ptr<GlobalTableFunctionState> RestCallsClearInit(ClientContext &context,
                                                               TableFunctionInitInput &input) {
	return make_uniq<RestCallsClearGlobalState>();
}

static void RestCallsClearScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<RestCallsClearGlobalState>();
	if (state.done) {
		output.SetCardinality(0);
		return;
	}
	state.done = true;
	auto removed = data.bind_data->Cast<RestCallsBindData>().log->Clear();
	output.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(removed)));
	output.SetCardinality(1);
}

static void BuildFunctions(const shared_ptr<RestCallLog> &log, TableFunction &calls, TableFunction &clear) {
	auto info = make_shared_ptr<RestCallLogInfo>();
	info->log = log;
	calls = TableFunction("rest_ext_calls", {}, RestCallsScan, RestCallsBind, RestCallsInit);
	calls.function_info = info;
	clear = TableFunction("rest_ext_calls_clear", {}, RestCallsClearScan, RestCallsClearBind, RestCallsClearInit);
	clear.function_info = info;
}

static shared_ptr<RestCallLog> CreateLog(DatabaseInstance &db) {
	auto log = make_shared_ptr<RestCallLog>();
	std::lock_guard<std::mutex> guard(registry_lock);
	registry[&db] = log;
	return log;
}

#ifdef REST_EXT_HAS_EXTENSION_LOADER
void RegisterRestCallLog(ExtensionLoader &loader) {
	auto log = CreateLog(loader.GetDatabaseInstance());
	TableFunction calls, clear;
	BuildFunctions(log, calls, clear);
	loader.RegisterFunction(calls);
	loader.RegisterFunction(clear);
}
#else
void RegisterRestCallLog(DatabaseInstance &db) {
	auto log = CreateLog(db);
	TableFunction calls, clear;
	BuildFunctions(log, calls, clear);
	ExtensionUtil::RegisterFunction(db, calls);
	ExtensionUtil::RegisterFunction(db, clear);
}
#endif

} // namespace duckdb
