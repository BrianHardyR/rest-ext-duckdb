#include "rest_attach.hpp"

#include "http_client.hpp"
#include "json_helpers.hpp"
#include "rest_attachment_catalog.hpp"
#include "rest_fetch_function.hpp"
#include "rest_namespace_catalog.hpp"
#include "rest_secrets.hpp"
#include "spec_import.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

#include <algorithm>

namespace duckdb {

// ATTACH's parenthesized options, e.g. `(TYPE rest_ext, FORMAT 'openapi')`, preserve whatever
// letter-casing the user typed for each option NAME (unlike CREATE SECRET's options, which DuckDB
// lowercases for you) - so a plain `options.options.find("format")` would miss `FORMAT`/`Format`.
// This does the case-insensitive search ourselves, and returns the option's value as plain text.
static string FindAttachOptionCI(const AttachOptions &options, const string &key) {
	for (auto &entry : options.options) {
		if (StringUtil::CIEquals(entry.first, key)) {
			return entry.second.ToString();
		}
	}
	return string();
}

// Reads a local file's full contents from disk.
//
// This is NOT used for http(s) URLs, even though our own HTTP client could easily fetch one -
// DuckDB core itself intercepts ATTACH before our own `attach` callback ever runs: any ATTACH
// path string starting with a recognized remote-file prefix (http://, https://, s3://, ...) is
// unconditionally required to come with the `httpfs` extension loaded
// (DatabaseManager::AttachDatabase -> FileSystem::IsRemoteFile, in duckdb core, checked purely by
// string prefix - it has no awareness of our own TYPE or attach callback, and there is no way to
// opt out of it). So a spec URL can never be put directly in the ATTACH path position without
// forcing every user to install and load httpfs too, even though we don't actually need anything
// httpfs provides. Instead, a remote spec is fetched via the separate SPEC_URL option (see below),
// whose value never passes through that check at all, since only the path itself is inspected.
static string ReadSpecFile(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
	auto file_size = fs.GetFileSize(*handle);
	string content(file_size, '\0');
	fs.Read(*handle, const_cast<char *>(content.data()), file_size);
	return content;
}

// Pulls the individual "key=value" settings out of the ATTACH path string, e.g.
//   'url=https://api.example.com headers={"Accept":"application/json"} method=GET'
//
// We can't just split on whitespace, because a JSON value like headers={"Accept": "application/
// json"} can itself contain spaces. Instead, we find where each recognized "key=" marker starts,
// sort those positions, and treat everything between one marker and the next as that key's value
// (trimming any surrounding whitespace). This means a value is free to contain spaces, braces,
// commas, etc. - anything except the literal text of another recognized key.
static void ParseRestAttachPath(const string &path, string &url, string &headers_json, string &method,
                                string &resources_json) {
	static const vector<string> keys = {"url=", "headers=", "method=", "resources="};

	vector<std::pair<string, idx_t>> positions;
	for (auto &key : keys) {
		auto pos = path.find(key);
		if (pos != string::npos) {
			positions.emplace_back(key, pos);
		}
	}
	std::sort(positions.begin(), positions.end(),
	          [](const std::pair<string, idx_t> &a, const std::pair<string, idx_t> &b) { return a.second < b.second; });

	for (idx_t i = 0; i < positions.size(); i++) {
		auto &key = positions[i].first;
		auto start = positions[i].second + key.size();
		auto end = (i + 1 < positions.size()) ? positions[i + 1].second : path.size();
		auto value = path.substr(start, end - start);
		StringUtil::Trim(value);

		if (key == "url=") {
			url = value;
		} else if (key == "headers=") {
			headers_json = value;
		} else if (key == "method=") {
			method = value;
		} else if (key == "resources=") {
			resources_json = value;
		}
	}
}

// The `attach` callback: this runs once, when a user executes an ATTACH statement naming our
// extension. It has to return a Catalog (DuckDB will throw an internal error if we return
// nullptr), which is why even single-endpoint mode - which doesn't really need a "database" in any
// meaningful sense - still builds a small placeholder one (see rest_attachment_catalog.hpp).
static unique_ptr<Catalog> RestAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                      AttachedDatabase &db, const string &name, AttachInfo &info,
                                      AttachOptions &options) {
	// FORMAT switches ATTACH into "import from an OpenAPI spec document" mode. See spec_import.hpp
	// for the full explanation. The spec document itself comes from one of two places:
	//   - SPEC_URL, an http(s) URL fetched with our own HTTP client - required for a REMOTE spec,
	//     since (see ReadSpecFile's comment) a URL can never go directly in the ATTACH path.
	//   - otherwise, the ATTACH path itself, treated as a local file path.
	auto format_option = FindAttachOptionCI(options, "format");
	if (!format_option.empty()) {
		if (!StringUtil::CIEquals(format_option, "openapi")) {
			throw BinderException("rest_ext: unrecognized FORMAT \"%s\" (expected 'openapi')", format_option);
		}

		auto spec_url = FindAttachOptionCI(options, "spec_url");
		string spec_document;
		string spec_origin; // "scheme://host[:port]" the spec was fetched from, if known - used to
		                    // resolve a RELATIVE "servers" URL (see ParseOpenApiSpec's header comment)
		if (!spec_url.empty()) {
			spec_document = PerformRestCall(spec_url, "GET", "{}", "{}", "{}");
			string unused_path;
			HTTPUtil::DecomposeURL(spec_url, unused_path, spec_origin);
		} else {
			spec_document = ReadSpecFile(context, info.path);
		}

		auto base_url_override = FindAttachOptionCI(options, "base_url");
		auto resources = ParseOpenApiSpec(spec_document, base_url_override, spec_origin);

		// A spec document rarely embeds real credentials - CREATE SECRET (see rest_secrets.hpp)
		// still applies here exactly like it does for hand-written namespace mode, matched
		// independently per resource URL since a spec can span multiple hosts.
		for (auto &resource : resources) {
			resource.headers_json = ResolveHeaders(context, resource.url, resource.headers_json);
		}

		info.path = ":memory:";
		return CreateRestNamespaceCatalog(db, std::move(resources));
	}

	string url;
	string headers_json = "{}";
	string method = "GET";
	string resources_json;
	ParseRestAttachPath(info.path, url, headers_json, method, resources_json);

	if (!resources_json.empty()) {
		// Namespace mode. resources_json is itself a flat JSON object, e.g.
		// {"Projects":"https://...","Usage":"https://..."} - we parse it the exact same way we'd
		// parse a headers or query-params object, since it's shaped the same way (flat string ->
		// string pairs), just with a different meaning (resource name -> full URL). Every resource
		// shares the same method= here, since the manual mini-language has no way to give one
		// resource its own method - use FORMAT with a real spec if you need mixed methods.
		vector<RestResourceConfig> resources;
		ParseFlatJsonObject(resources_json, [&](const string &resource_name, const string &resource_url) {
			// Headers are resolved (secret + inline merge) separately PER resource URL, not once
			// for the whole namespace - a namespace's resources can span different hosts (e.g.
			// GCP's various services), and different hosts may match different stored secrets.
			resources.push_back(
			    {resource_name, resource_url, method, ResolveHeaders(context, resource_url, headers_json)});
		});
		if (resources.empty()) {
			throw BinderException("rest_ext ATTACH: resources={...} must be a non-empty JSON object mapping "
			                      "name -> URL, e.g. resources={\"Projects\":\"https://.../v1/projects\"}");
		}

		// ":memory:" tells DuckDB "there's no real file behind this attachment" - we're not
		// backed by anything on disk.
		info.path = ":memory:";
		return CreateRestNamespaceCatalog(db, std::move(resources));
	}

	// Single-endpoint mode.
	if (url.empty()) {
		throw BinderException(
		    "rest_ext ATTACH requires a url=... component in the path, e.g. "
		    "ATTACH 'url=https://api.example.com headers={\"Accept\":\"application/json\"} method=GET' AS "
		    "myapi (TYPE rest_ext)");
	}

	auto fetch_info = make_shared_ptr<RestFetchInfo>();
	fetch_info->url = url;
	fetch_info->headers_json = ResolveHeaders(context, url, headers_json);
	fetch_info->method = method;

	// Build a TableFunction named after the ATTACH alias itself (e.g. "myapi"), so
	// `SELECT * FROM myapi(...)` resolves to it directly - see rest_attachment_catalog.hpp's
	// header comment for why this has to be registered in the shared system catalog rather than
	// in some catalog of our own.
	TableFunction call_function(Identifier(name), {LogicalType::VARCHAR, LogicalType::VARCHAR}, RestFetchFunction,
	                            RestFetchBind, RestFetchInitGlobal);
	call_function.function_info = fetch_info;

	CreateTableFunctionInfo create_info(call_function);
	Catalog::GetSystemCatalog(context).CreateTableFunction(context, create_info);

	info.path = ":memory:";
	return CreateRestAttachmentCatalog(db, name);
}

static unique_ptr<TransactionManager> RestCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                   AttachedDatabase &db, Catalog &catalog) {
	// Single-endpoint mode's placeholder Catalog is a DuckCatalog subclass (see
	// rest_attachment_catalog.hpp), and DuckDB's ready-made DuckTransactionManager only works with
	// a DuckCatalog - it has an internal check that refuses anything else. Namespace mode's
	// Catalog is NOT a DuckCatalog (it doesn't need real on-disk storage), so it needs our own
	// minimal TransactionManager instead. IsDuckCatalog() is exactly the flag DuckDB itself uses
	// to tell the two situations apart, so we just check the same thing here.
	if (catalog.IsDuckCatalog()) {
		return make_uniq<DuckTransactionManager>(db);
	}
	return CreateRestTransactionManager(db);
}

// The StorageExtension struct itself is just two function pointers - this constructor is where we
// point them at the functions defined above.
struct RestExtStorageExtension : public StorageExtension {
	RestExtStorageExtension() {
		attach = RestAttach;
		create_transaction_manager = RestCreateTransactionManager;
	}
};

void RegisterRestExtStorageExtension(DatabaseInstance &db) {
	auto &config = DBConfig::GetConfig(db);
	StorageExtension::Register(config, "rest_ext", make_shared_ptr<RestExtStorageExtension>());
}

} // namespace duckdb
