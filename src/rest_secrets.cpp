#include "rest_secrets.hpp"

#include "json_helpers.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

// This is what actually runs when a user executes `CREATE SECRET (TYPE rest_ext_headers, ...)`.
// DuckDB calls it with `input` holding whatever named parameters the user supplied (here, just
// `headers`), and we build a generic "KeyValueSecret" - DuckDB's built-in bag-of-values secret
// representation, the same one the core `http` secret type uses - to hold it.
static unique_ptr<BaseSecret> CreateRestExtHeadersSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	secret->TrySetValue("headers", input);

	// Mark "headers" as sensitive so DuckDB shows "redacted" instead of the real value whenever
	// this secret is displayed (e.g. in the duckdb_secrets() system view). We redact the WHOLE map
	// rather than trying to guess which individual keys look sensitive - a header bag is exactly
	// where bearer tokens and API keys live, and there's no reliable way to tell an "Authorization"
	// key from an innocuous one by name alone.
	secret->redact_keys = {"headers"};
	return std::move(secret);
}

#ifdef REST_EXT_HAS_EXTENSION_LOADER
void RegisterRestExtHeadersSecretType(ExtensionLoader &loader) {
	// Step 1: register the TYPE itself (the name "rest_ext_headers" that CREATE SECRET's TYPE
	// clause refers to).
	SecretType secret_type;
	secret_type.name = "rest_ext_headers";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);

	// Step 2: register a "provider" for that type - the actual function that builds one when a
	// user runs CREATE SECRET. DuckDB secrets support multiple providers per type (e.g. "config"
	// for values typed directly into the SQL statement, vs "env" for reading environment
	// variables); we only need the simplest one, "config".
	CreateSecretFunction secret_function;
	secret_function.secret_type = "rest_ext_headers";
	secret_function.provider = "config";
	secret_function.function = CreateRestExtHeadersSecretFromConfig;
	// Declaring "headers" as a MAP(VARCHAR, VARCHAR) here is what makes
	// `HEADERS MAP {'Authorization': 'Bearer ...'}` valid syntax and rejects anything else typed
	// after HEADERS that isn't a string-to-string map.
	secret_function.named_parameters["headers"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	loader.RegisterFunction(secret_function);
}
#else
void RegisterRestExtHeadersSecretType(DatabaseInstance &db) {
	// Same two steps as the ExtensionLoader path above, just via the older, free-function
	// ExtensionUtil API that DuckDB <= v1.4.x still uses.
	SecretType secret_type;
	secret_type.name = "rest_ext_headers";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	ExtensionUtil::RegisterSecretType(db, secret_type);

	CreateSecretFunction secret_function;
	secret_function.secret_type = "rest_ext_headers";
	secret_function.provider = "config";
	secret_function.function = CreateRestExtHeadersSecretFromConfig;
	secret_function.named_parameters["headers"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	ExtensionUtil::RegisterFunction(db, secret_function);
}
#endif

string ResolveHeaders(ClientContext &context, const string &url, const string &inline_headers_json) {
	case_insensitive_map_t<string> merged;

	// Ask DuckDB's secret manager: "of all the rest_ext_headers secrets registered, which one (if
	// any) best matches this URL?" Matching works by longest SCOPE-prefix, same as the built-in
	// `http`/S3 secrets use - e.g. a secret scoped to "https://api.example.com/v1" would be
	// preferred over one scoped to just "https://api.example.com" when both match.
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = secret_manager.LookupSecret(transaction, url, "rest_ext_headers");
	if (match.HasMatch()) {
		auto &kv_secret = dynamic_cast<const KeyValueSecret &>(match.GetSecret());
		Value headers_value;
		if (kv_secret.TryGetValue("headers", headers_value)) {
			// A MAP value's internal representation is a list of {key, value} structs - this is
			// just how DuckDB represents any MAP as a plain Value, nothing specific to secrets.
			for (auto &entry : MapValue::GetChildren(headers_value)) {
				auto &kv = StructValue::GetChildren(entry);
				merged[kv[0].ToString()] = kv[1].ToString();
			}
		}
	}

	// Now layer the inline headers= from the ATTACH string on top. Since we assign into the same
	// map by key, a key that exists in both simply gets overwritten here - meaning inline wins.
	ParseFlatJsonObject(inline_headers_json, [&](const string &key, const string &value) { merged[key] = value; });

	return BuildFlatJsonObject(merged);
}

} // namespace duckdb
