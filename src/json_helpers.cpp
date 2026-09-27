#include "json_helpers.hpp"

using namespace duckdb_yyjson; // NOLINT - lets us write "yyjson_read(...)" instead of the fully
                                // qualified "duckdb_yyjson::yyjson_read(...)" everywhere below.

namespace duckdb {

void ParseFlatJsonObject(const string &json, const std::function<void(const string &, const string &)> &callback) {
	if (json.empty()) {
		return;
	}

	// yyjson_read() parses the text into an in-memory tree and hands back a "document" handle.
	// If the text isn't valid JSON at all, it returns a null pointer instead of throwing - so we
	// check for that ourselves.
	auto doc = yyjson_read(json.c_str(), json.size(), 0);
	if (!doc) {
		throw InvalidInputException("rest_ext: failed to parse JSON object: %s", json);
	}

	// Every yyjson document has one "root" value. We only accept a JSON object as the root here
	// (query params and headers are always a flat {"key": "value", ...} object).
	auto root = yyjson_doc_get_root(doc);
	if (!root || !yyjson_is_obj(root)) {
		yyjson_doc_free(doc);
		throw InvalidInputException("rest_ext: expected a flat JSON object, got: %s", json);
	}

	// Walk every key/value pair in the object, in the order they appear in the JSON text.
	yyjson_val *key;
	yyjson_val *val;
	yyjson_obj_iter iter = yyjson_obj_iter_with(root);
	while ((key = yyjson_obj_iter_next(&iter))) {
		val = yyjson_obj_iter_get_val(key);
		string key_str(yyjson_get_str(key), yyjson_get_len(key));

		string value_str;
		if (yyjson_is_str(val)) {
			value_str = string(yyjson_get_str(val), yyjson_get_len(val));
		} else if (yyjson_is_num(val)) {
			value_str = StringUtil::Format("%g", yyjson_get_num(val));
		} else if (yyjson_is_bool(val)) {
			value_str = yyjson_get_bool(val) ? "true" : "false";
		} else {
			// Nested object/array/null - not something a flat header/query-param bag should
			// contain. We just skip it rather than erroring, to stay forgiving.
			continue;
		}
		callback(key_str, value_str);
	}

	// yyjson documents are plain heap allocations we own - we must free the one we created above.
	yyjson_doc_free(doc);
}

string BuildFlatJsonObject(const case_insensitive_map_t<string> &entries) {
	// This is the "write" side of yyjson: yyjson_mut_doc_new() creates an empty, buildable
	// document (distinct from the read-only "yyjson_doc" used elsewhere in this file).
	auto doc = yyjson_mut_doc_new(nullptr);
	auto obj = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, obj);

	for (auto &entry : entries) {
		yyjson_mut_obj_add_str(doc, obj, entry.first.c_str(), entry.second.c_str());
	}

	size_t len = 0;
	auto *raw = yyjson_mut_write(doc, 0, &len);
	string result = raw ? string(raw, len) : "{}";
	free(raw); // NOLINT - yyjson_mut_write hands back a plain malloc()'d buffer we must free.
	yyjson_mut_doc_free(doc);
	return result;
}

LogicalType MergeJsonTypes(const LogicalType &a, const LogicalType &b) {
	if (a == b) {
		return a;
	}
	// SQLNULL is our "don't know yet" placeholder (used before we've seen any real value) - if
	// either side is still unknown, the other side's type wins outright.
	if (a.id() == LogicalTypeId::SQLNULL) {
		return b;
	}
	if (b.id() == LogicalTypeId::SQLNULL) {
		return a;
	}
	// A whole number (BIGINT) and a decimal number (DOUBLE) together should widen to DOUBLE, so
	// no precision is lost either way.
	if ((a.id() == LogicalTypeId::BIGINT && b.id() == LogicalTypeId::DOUBLE) ||
	    (a.id() == LogicalTypeId::DOUBLE && b.id() == LogicalTypeId::BIGINT)) {
		return LogicalType::DOUBLE;
	}
	if (a.id() == LogicalTypeId::STRUCT && b.id() == LogicalTypeId::STRUCT) {
		// Two STRUCTs merge into one STRUCT holding the UNION of both sets of fields. A field
		// present in both sides gets its own type merged recursively (it might itself be a
		// STRUCT or LIST that also needs combining).
		auto merged = StructType::GetChildTypes(a);
		unordered_map<string, idx_t> index_by_name;
		for (idx_t i = 0; i < merged.size(); i++) {
			index_by_name[merged[i].first] = i;
		}
		for (auto &b_child : StructType::GetChildTypes(b)) {
			auto &b_name = b_child.first;
			auto it = index_by_name.find(b_name);
			if (it == index_by_name.end()) {
				index_by_name[b_name] = merged.size();
				merged.push_back(b_child);
			} else {
				merged[it->second].second = MergeJsonTypes(merged[it->second].second, b_child.second);
			}
		}
		return LogicalType::STRUCT(std::move(merged));
	}
	if (a.id() == LogicalTypeId::LIST && b.id() == LogicalTypeId::LIST) {
		return LogicalType::LIST(MergeJsonTypes(ListType::GetChildType(a), ListType::GetChildType(b)));
	}
	// Anything else is a genuine clash (e.g. one row had a number, another had a whole object for
	// the same key) - fall back to VARCHAR so a single unusual row doesn't blow up the whole
	// query. JsonToValue below knows how to put a non-string value into a VARCHAR column too.
	return LogicalType::VARCHAR;
}

LogicalType InferJsonType(yyjson_val *val) {
	if (!val || yyjson_is_null(val)) {
		return LogicalType::SQLNULL;
	}
	if (yyjson_is_bool(val)) {
		return LogicalType::BOOLEAN;
	}
	if (yyjson_is_int(val)) {
		return LogicalType::BIGINT;
	}
	if (yyjson_is_num(val)) {
		// A JSON number that isn't a plain integer (has a decimal point or exponent).
		return LogicalType::DOUBLE;
	}
	if (yyjson_is_str(val)) {
		return LogicalType::VARCHAR;
	}
	if (yyjson_is_arr(val)) {
		// Figure out ONE type that fits every element of the array, by merging them together one
		// at a time - e.g. [1, 2, 3.5] ends up typed as a LIST of DOUBLE.
		LogicalType child_type = LogicalType::SQLNULL;
		size_t idx, max;
		yyjson_val *elem;
		yyjson_arr_foreach(val, idx, max, elem) {
			child_type = MergeJsonTypes(child_type, InferJsonType(elem));
		}
		if (child_type.id() == LogicalTypeId::SQLNULL) {
			child_type = LogicalType::VARCHAR; // an empty array - nothing to infer from, so default
			                                    // to VARCHAR rather than leaving it "unknown".
		}
		return LogicalType::LIST(child_type);
	}
	if (yyjson_is_obj(val)) {
		// A JSON object becomes a STRUCT: one named field per JSON key, each field's type worked
		// out recursively (so nested objects/arrays are handled automatically).
		child_list_t<LogicalType> children;
		yyjson_val *key;
		yyjson_val *v;
		yyjson_obj_iter iter = yyjson_obj_iter_with(val);
		while ((key = yyjson_obj_iter_next(&iter))) {
			v = yyjson_obj_iter_get_val(key);
			children.emplace_back(string(yyjson_get_str(key), yyjson_get_len(key)), InferJsonType(v));
		}
		if (children.empty()) {
			// An empty JSON object ({}) has no keys to build a STRUCT from - give it one
			// placeholder field so it's still a valid (if useless) STRUCT type.
			children.emplace_back("value", LogicalType::VARCHAR);
		}
		return LogicalType::STRUCT(std::move(children));
	}
	return LogicalType::VARCHAR;
}

// Serializes an arbitrary JSON value back to JSON text. Used only as a fallback inside
// JsonToValue, for the case where a column was typed VARCHAR because of a conflict between rows,
// but this particular row's value isn't itself a string (e.g. one row had a number where every
// other row had a string) - rather than losing the value, we show it as its own JSON text.
static string YyjsonValToRawText(yyjson_val *val) {
	size_t len = 0;
	auto *raw = yyjson_val_write(val, 0, &len);
	if (!raw) {
		return string();
	}
	string result(raw, len);
	free(raw); // NOLINT
	return result;
}

Value JsonToValue(yyjson_val *val, const LogicalType &type) {
	if (!val || yyjson_is_null(val)) {
		// Value(type) with no other arguments constructs a properly-typed SQL NULL.
		return Value(type);
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return yyjson_is_bool(val) ? Value::BOOLEAN(yyjson_get_bool(val)) : Value(type);
	case LogicalTypeId::BIGINT:
		return yyjson_is_int(val) ? Value::BIGINT(yyjson_get_sint(val)) : Value(type);
	case LogicalTypeId::DOUBLE:
		return yyjson_is_num(val) ? Value::DOUBLE(yyjson_get_num(val)) : Value(type);
	case LogicalTypeId::VARCHAR:
		if (yyjson_is_str(val)) {
			return Value(string(yyjson_get_str(val), yyjson_get_len(val)));
		}
		// The column is VARCHAR but this particular value isn't a JSON string (see the comment on
		// YyjsonValToRawText above for why this can legitimately happen).
		return Value(YyjsonValToRawText(val));
	case LogicalTypeId::LIST: {
		auto &child_type = ListType::GetChildType(type);
		vector<Value> elements;
		if (yyjson_is_arr(val)) {
			size_t idx, max;
			yyjson_val *elem;
			yyjson_arr_foreach(val, idx, max, elem) {
				elements.push_back(JsonToValue(elem, child_type));
			}
		}
		return Value::LIST(child_type, std::move(elements));
	}
	case LogicalTypeId::STRUCT: {
		// Build one Value per field the STRUCT type expects, looking each one up by name in the
		// JSON object. A field that's missing from this particular JSON value (e.g. an optional
		// key some rows have and others don't) just becomes NULL.
		child_list_t<Value> struct_values;
		for (auto &child : StructType::GetChildTypes(type)) {
			auto &child_name = child.first;
			yyjson_val *child_val =
			    yyjson_is_obj(val) ? yyjson_obj_getn(val, child_name.c_str(), child_name.size()) : nullptr;
			struct_values.emplace_back(child.first, JsonToValue(child_val, child.second));
		}
		return Value::STRUCT(std::move(struct_values));
	}
	default:
		return Value(type);
	}
}

} // namespace duckdb
