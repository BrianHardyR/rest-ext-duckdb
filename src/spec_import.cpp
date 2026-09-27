#include "spec_import.hpp"

#include "json_helpers.hpp"

#include "duckdb/common/case_insensitive_map.hpp"

#include "yyjson.hpp"

#include <cctype>

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

// Turns an arbitrary string into something that's safe to use as an unquoted SQL identifier, by
// replacing every character that isn't a letter/digit/underscore with an underscore. Used when we
// have to invent a resource name ourselves (an OpenAPI operation with no operationId) rather than
// use one the spec author already chose.
static string SanitizeIdentifier(string s) {
	for (auto &c : s) {
		if (!isalnum(static_cast<unsigned char>(c)) && c != '_') {
			c = '_';
		}
	}
	return s;
}

static string GetJsonString(yyjson_val *obj, const char *key) {
	if (!obj || !yyjson_is_obj(obj)) {
		return string();
	}
	auto val = yyjson_obj_get(obj, key);
	if (!val || !yyjson_is_str(val)) {
		return string();
	}
	return string(yyjson_get_str(val), yyjson_get_len(val));
}

// Replaces every {name} placeholder in `text` with the value looked up in `vars`, leaving a
// placeholder untouched if `vars` doesn't have a matching entry for it (rather than guessing or
// erroring - a server URL that still has an unresolved {placeholder} in it will simply fail to
// connect later, which is a clear enough signal something needs a BASE_URL override instead).
static string SubstitutePlaceholders(string text, const case_insensitive_map_t<string> &vars) {
	for (auto &entry : vars) {
		string placeholder = "{" + entry.first + "}";
		idx_t pos = 0;
		while ((pos = text.find(placeholder, pos)) != string::npos) {
			text.replace(pos, placeholder.size(), entry.second);
			pos += entry.second.size();
		}
	}
	return text;
}

// Works out the one base URL every operation's path gets appended to.
//
//   OpenAPI 3.x uses a "servers" array, e.g.
//     servers: [{"url": "https://{environment}.example.com/{version}",
//                "variables": {"environment": {"default": "api"}, "version": {"default": "v1"}}}]
//   We use the first server entry, and substitute any {variable} placeholders in its URL using
//   each variable's own declared "default" value - a server URL with unresolved variables would
//   otherwise be useless, since we have no other source to pick a value from.
//
//   OpenAPI 2.0 ("Swagger") instead uses three separate fields: "host", "basePath", and
//   "schemes" (e.g. host: "api.example.com", basePath: "/v1", schemes: ["https"]).
static string DetermineOpenApiBaseUrl(yyjson_val *root) {
	auto servers = yyjson_obj_get(root, "servers");
	if (servers && yyjson_is_arr(servers) && yyjson_arr_size(servers) > 0) {
		auto first_server = yyjson_arr_get_first(servers);
		auto url = GetJsonString(first_server, "url");
		if (!url.empty()) {
			case_insensitive_map_t<string> defaults;
			auto variables = yyjson_obj_get(first_server, "variables");
			if (variables && yyjson_is_obj(variables)) {
				yyjson_val *var_key;
				yyjson_val *var_val;
				yyjson_obj_iter iter = yyjson_obj_iter_with(variables);
				while ((var_key = yyjson_obj_iter_next(&iter))) {
					var_val = yyjson_obj_iter_get_val(var_key);
					string var_name(yyjson_get_str(var_key), yyjson_get_len(var_key));
					defaults[var_name] = GetJsonString(var_val, "default");
				}
			}
			return SubstitutePlaceholders(url, defaults);
		}
	}

	auto host = GetJsonString(root, "host");
	if (!host.empty()) {
		string scheme = "https";
		auto schemes = yyjson_obj_get(root, "schemes");
		if (schemes && yyjson_is_arr(schemes) && yyjson_arr_size(schemes) > 0) {
			auto first_scheme = yyjson_arr_get_first(schemes);
			if (yyjson_is_str(first_scheme)) {
				scheme = string(yyjson_get_str(first_scheme), yyjson_get_len(first_scheme));
			}
		}
		return scheme + "://" + host + GetJsonString(root, "basePath");
	}

	return string();
}

vector<RestResourceConfig> ParseOpenApiSpec(const string &spec_json, const string &base_url_override,
                                            const string &spec_origin) {
	auto doc = yyjson_read(spec_json.c_str(), spec_json.size(), 0);
	if (!doc) {
		throw InvalidInputException("rest_ext: failed to parse OpenAPI spec as JSON");
	}
	auto root = yyjson_doc_get_root(doc);
	if (!root || !yyjson_is_obj(root) || (!yyjson_obj_get(root, "openapi") && !yyjson_obj_get(root, "swagger"))) {
		yyjson_doc_free(doc);
		throw InvalidInputException("rest_ext: expected an OpenAPI document (a JSON object with a top-level "
		                            "\"openapi\" or \"swagger\" version key)");
	}

	string base_url = !base_url_override.empty() ? base_url_override : DetermineOpenApiBaseUrl(root);
	if (base_url.empty()) {
		yyjson_doc_free(doc);
		throw InvalidInputException(
		    "rest_ext: could not determine a base URL from the OpenAPI spec's \"servers\"/\"host\" fields - "
		    "pass one explicitly, e.g. ATTACH '...' AS x (TYPE rest_ext, FORMAT 'openapi', BASE_URL "
		    "'https://api.example.com')");
	}

	// OpenAPI explicitly permits a "servers" URL to be RELATIVE (e.g. just "/api/v3", meaning
	// "wherever this spec document itself is hosted") - a real, common pattern, not a malformed
	// spec (the official Swagger Petstore v3 spec does exactly this). If we know where the spec
	// came from (spec_origin, only set when it was fetched via SPEC_URL rather than a local
	// file), resolve the relative URL against that; otherwise this is unresolvable and we say so
	// clearly rather than silently building a broken URL with no host in it at all.
	bool base_url_is_absolute = StringUtil::StartsWith(base_url, "http://") || StringUtil::StartsWith(base_url, "https://");
	if (!base_url_is_absolute) {
		if (!spec_origin.empty()) {
			base_url = spec_origin + (StringUtil::StartsWith(base_url, "/") ? "" : "/") + base_url;
		} else {
			yyjson_doc_free(doc);
			throw InvalidInputException(
			    "rest_ext: the OpenAPI spec's base URL (\"%s\") is relative, and there's no known origin to "
			    "resolve it against (only spec documents fetched via SPEC_URL have one) - pass an absolute "
			    "BASE_URL explicitly, e.g. ATTACH '...' AS x (TYPE rest_ext, FORMAT 'openapi', BASE_URL "
			    "'https://api.example.com')",
			    base_url);
		}
	}

	// The HTTP methods OpenAPI allows as keys directly under a path - only the ones our own
	// http_client.cpp actually knows how to send are worth extracting here.
	static const vector<string> methods = {"get", "post", "put", "patch", "delete"};

	vector<RestResourceConfig> resources;
	case_insensitive_map_t<idx_t> name_counts; // guards against two operations landing on the same name

	auto paths = yyjson_obj_get(root, "paths");
	if (paths && yyjson_is_obj(paths)) {
		// Every key under "paths" is a URL path template, e.g. "/pets/{petId}"; every value is an
		// object whose own keys are HTTP methods, e.g. {"get": {...}, "post": {...}}.
		yyjson_val *path_key;
		yyjson_val *path_val;
		yyjson_obj_iter path_iter = yyjson_obj_iter_with(paths);
		while ((path_key = yyjson_obj_iter_next(&path_iter))) {
			path_val = yyjson_obj_iter_get_val(path_key);
			string path_str(yyjson_get_str(path_key), yyjson_get_len(path_key));
			if (!yyjson_is_obj(path_val)) {
				continue;
			}

			for (auto &method : methods) {
				auto operation = yyjson_obj_getn(path_val, method.c_str(), method.size());
				if (!operation || !yyjson_is_obj(operation)) {
					continue; // this path doesn't define this particular HTTP method
				}

				// Prefer the spec author's own chosen name (operationId) - it's usually a much
				// nicer SQL identifier than anything we'd invent, e.g. "listPets" vs "get__pets".
				string resource_name = GetJsonString(operation, "operationId");
				if (resource_name.empty()) {
					resource_name = SanitizeIdentifier(method + "_" + path_str);
				}

				// Two different operations landing on the same name (a spec with a genuine
				// operationId clash, or two sanitized fallback names colliding) would otherwise
				// silently overwrite one another later when the schema is built - number the
				// later ones instead so every operation stays reachable.
				auto &count = name_counts[resource_name];
				count++;
				if (count > 1) {
					resource_name += "_" + std::to_string(count);
				}

				RestResourceConfig resource;
				resource.name = resource_name;
				// {parameter} placeholders in the path (e.g. "/pets/{petId}") are left exactly as
				// they appear in the spec - see http_client.hpp's PerformRestCall, which fills
				// them in per-call from whatever the caller passes as query_params.
				resource.url = base_url + path_str;
				resource.method = StringUtil::Upper(method);
				resource.headers_json = "{}"; // security schemes aren't parsed in this pass - use
				                              // a CREATE SECRET (see rest_secrets.hpp) for auth
				resources.push_back(std::move(resource));
			}
		}
	}

	yyjson_doc_free(doc);
	if (resources.empty()) {
		throw InvalidInputException("rest_ext: OpenAPI spec had no usable GET/POST/PUT/PATCH/DELETE operations "
		                            "under \"paths\"");
	}
	return resources;
}

} // namespace duckdb
