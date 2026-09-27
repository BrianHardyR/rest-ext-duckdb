#include "rest_attachment_catalog.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"

namespace duckdb {

// DuckCatalog is DuckDB's own normal, full-featured catalog implementation - the same one an
// ordinary DuckDB database file uses. We subclass it (rather than building a from-scratch Catalog
// the way rest_namespace_catalog.cpp does) purely so we get a fully working, boring catalog for
// free and only need to override the one thing we actually care about: cleanup on DETACH.
class RestAttachmentCatalog : public DuckCatalog {
public:
	RestAttachmentCatalog(AttachedDatabase &db, string function_name_p)
	    : DuckCatalog(db), function_name(std::move(function_name_p)) {
	}

	string GetCatalogType() override {
		return "rest_ext";
	}

	// Called automatically when the user runs `DETACH myapi`. Without this override, the
	// `myapi(...)` table function we registered into the system catalog at ATTACH time (see
	// rest_attach.cpp) would just keep existing forever - DETACH only removes the placeholder
	// Catalog object itself, it has no way to know we'd also stashed something else in the system
	// catalog unless we tell it here.
	void OnDetach(ClientContext &context) override {
		DropInfo drop_info;
		drop_info.type = CatalogType::TABLE_FUNCTION_ENTRY;
		// RETURN_NULL (rather than the default of throwing) means "if it's somehow already gone,
		// that's fine, don't error" - DETACH should never fail just because cleanup found nothing
		// left to clean up.
		drop_info.if_not_found = OnEntryNotFound::RETURN_NULL;
		// The function was registered as "internal" (a detail of how CreateTableFunctionInfo
		// works), and DuckDB normally protects internal/built-in entries from being dropped by
		// accident - allow_drop_internal says "yes, I really do mean to drop this one".
		drop_info.allow_drop_internal = true;
		drop_info.name = function_name;
		Catalog::GetSystemCatalog(context).DropEntry(context, drop_info);
	}

private:
	string function_name;
};

unique_ptr<Catalog> CreateRestAttachmentCatalog(AttachedDatabase &db, string function_name) {
	auto catalog = make_uniq<RestAttachmentCatalog>(db, std::move(function_name));
	catalog->Initialize(false);
	return std::move(catalog);
}

} // namespace duckdb
