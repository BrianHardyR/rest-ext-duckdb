#include "rest_namespace_catalog.hpp"

#include "rest_fetch_function.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/transaction/transaction.hpp"

namespace duckdb {

// A small helper so every "we don't support this" override below can share one message, instead
// of repeating similar text 11 times. `[[noreturn]]` just tells the compiler this function always
// throws and never returns normally, which quiets down "missing return statement" warnings at
// every call site.
[[noreturn]] static void ThrowNamespaceUnsupported(const string &schema_name, const string &operation) {
	throw NotImplementedException("rest_ext namespace \"%s\" is a fixed, read-only view of configured REST "
	                              "resources; %s is not supported",
	                              schema_name, operation);
}

// --- What is a "Schema" in DuckDB? ---
//
// A Catalog (the whole attached "database") is organized into one or more Schemas, and each
// Schema holds the actual named things you can query: tables, views, functions, and so on -
// `information_schema`/`pg_catalog`/"main" are examples you may have seen in a normal DuckDB
// database. Our namespace only ever needs exactly one schema (we call it "main", matching normal
// DuckDB convention), holding one table function per configured resource.
//
// SchemaCatalogEntry is DuckDB's base class for a schema. It has a lot of virtual methods, because
// a real schema needs to support CREATE TABLE, CREATE VIEW, CREATE INDEX, DROP, ALTER, and so on.
// Our schema is fixed and read-only (its contents are decided once, at ATTACH time, from the
// resources={...} the user gave us) - so every one of those "modify me" methods below just throws
// a clear error via ThrowNamespaceUnsupported instead of doing anything. The only methods that do
// real work are Scan/LookupEntry (used to find/list what resources exist) and the constructor
// (which builds one table function per resource up front).
class RestNamespaceSchemaEntry : public SchemaCatalogEntry {
public:
	RestNamespaceSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, const vector<RestResourceConfig> &resources)
	    : SchemaCatalogEntry(catalog, info) {
		for (auto &resource : resources) {
			// RestFetchInfo is how the resource's own url/headers/method reach the shared bind
			// function later (see rest_fetch_function.hpp for the full explanation of why it has
			// to travel this way).
			auto fetch_info = make_shared_ptr<RestFetchInfo>();
			fetch_info->url = resource.url;
			fetch_info->headers_json = resource.headers_json;
			fetch_info->method = resource.method;
			fetch_info->paging = resource.paging;

			// Build a TableFunction named after this resource (e.g. "Projects"), wired up to the
			// SAME bind/execute callbacks every other REST call in this extension uses.
			TableFunction fn(resource.name, {LogicalType::VARCHAR, LogicalType::VARCHAR},
			                 RestFetchFunction, RestFetchBind, RestFetchInitGlobal);
			fn.function_info = fetch_info;

			CreateTableFunctionInfo create_info(fn);
			resource_entries.emplace(resource.name, make_uniq<TableFunctionCatalogEntry>(catalog, *this, create_info));
		}
	}
	~RestNamespaceSchemaEntry() override = default;

	// Scan is how DuckDB enumerates "everything in this schema" - used for things like tab
	// completion or querying duckdb_functions(). We just report every resource we built above.
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override {
		Scan(type, callback);
	}
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override {
		if (type != CatalogType::TABLE_FUNCTION_ENTRY) {
			return; // we don't have any tables, views, etc. - only table functions
		}
		for (auto &entry : resource_entries) {
			callback(*entry.second);
		}
	}

	// LookupEntry is how DuckDB answers "does `gcp.Projects` actually exist?" while binding a
	// query - it's called with the name it's looking for, and we either find it in our map or
	// return nullptr to say "no such thing".
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override {
		if (lookup_info.GetCatalogType() != CatalogType::TABLE_FUNCTION_ENTRY) {
			return nullptr;
		}
		auto it = resource_entries.find(lookup_info.GetEntryName());
		return it == resource_entries.end() ? nullptr : it->second.get();
	}

	// Everything below is a "please modify this schema" request (CREATE TABLE, DROP, ALTER, ...).
	// Our schema's contents are fixed at ATTACH time, so all of these simply refuse with a clear
	// error rather than silently doing nothing or crashing.
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) override {
		ThrowNamespaceUnsupported(name, "CREATE INDEX");
	}
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction, CreateFunctionInfo &) override {
		ThrowNamespaceUnsupported(name, "CREATE FUNCTION");
	}
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction, BoundCreateTableInfo &) override {
		ThrowNamespaceUnsupported(name, "CREATE TABLE");
	}
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction, CreateViewInfo &) override {
		ThrowNamespaceUnsupported(name, "CREATE VIEW");
	}
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction, CreateSequenceInfo &) override {
		ThrowNamespaceUnsupported(name, "CREATE SEQUENCE");
	}
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) override {
		ThrowNamespaceUnsupported(name, "CREATE FUNCTION");
	}
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) override {
		ThrowNamespaceUnsupported(name, "CREATE COPY FUNCTION");
	}
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) override {
		ThrowNamespaceUnsupported(name, "CREATE PRAGMA FUNCTION");
	}
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction, CreateCollationInfo &) override {
		ThrowNamespaceUnsupported(name, "CREATE COLLATION");
	}
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction, CreateTypeInfo &) override {
		ThrowNamespaceUnsupported(name, "CREATE TYPE");
	}
	void DropEntry(ClientContext &, DropInfo &) override {
		ThrowNamespaceUnsupported(name, "DROP");
	}
	void Alter(CatalogTransaction, AlterInfo &) override {
		ThrowNamespaceUnsupported(name, "ALTER");
	}

private:
	// Looked up by resource name (e.g. "Projects"), case-insensitively - SQL identifiers are
	// normally case-insensitive, so `gcp.projects` and `gcp.Projects` should both work.
	case_insensitive_map_t<unique_ptr<TableFunctionCatalogEntry>> resource_entries;
};

// --- What is a "Catalog" in DuckDB? ---
//
// Every ATTACHed database is represented by one Catalog object - it's the top-level thing DuckDB
// asks "what's inside you?", and its answer is "here are my schemas" (see RestNamespaceSchemaEntry
// above). DuckDB's Catalog base class is a large interface (it has to support everything a real
// database needs: creating/dropping schemas, planning INSERT/UPDATE/DELETE, reporting its size on
// disk, and so on), but a read-only, in-memory, single-schema catalog like ours only needs to
// implement a handful of these meaningfully - the rest just throw, the same way the schema class
// above does.
class RestNamespaceCatalog : public Catalog {
public:
	RestNamespaceCatalog(AttachedDatabase &db, vector<RestResourceConfig> resources_p)
	    : Catalog(db), resources(std::move(resources_p)) {
	}
	~RestNamespaceCatalog() override = default;

	// Called once, right after construction, to actually build our one schema. (DuckDB calls this
	// separately from the constructor so that a catalog implementation can, in general, choose to
	// load its schema information lazily - we don't need to, but we still follow the same pattern
	// everyone else does.)
	void Initialize(bool load_builtin) override {
		CreateSchemaInfo info;
		info.schema = "main";
		schema = make_uniq<RestNamespaceSchemaEntry>(*this, info, resources);
	}

	string GetCatalogType() override {
		return "rest_ext";
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction, CreateSchemaInfo &) override {
		throw NotImplementedException("rest_ext namespaces have a single fixed schema; CREATE SCHEMA is not "
		                              "supported");
	}

	// LookupSchema is how DuckDB finds our one schema, regardless of what name it asked for -
	// there's only ever one, so we just hand it back (unless we haven't been Initialize()'d yet,
	// which should never actually happen in practice).
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction, const EntryLookupInfo &,
	                                              OnEntryNotFound if_not_found) override {
		if (schema) {
			return schema.get();
		}
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException("rest_ext namespace \"%s\" was not initialized", GetName());
		}
		return nullptr;
	}

	void ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) override {
		if (schema) {
			callback(*schema);
		}
	}

	// These four "Plan..." methods are how DuckDB would execute CREATE TABLE AS/INSERT/DELETE/
	// UPDATE against this catalog. We're read-only, so none of them make sense here.
	PhysicalOperator &PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
	                                    PhysicalOperator &) override {
		throw NotImplementedException("rest_ext namespaces are read-only");
	}
	PhysicalOperator &PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
	                             optional_ptr<PhysicalOperator>) override {
		throw NotImplementedException("rest_ext namespaces are read-only");
	}
	PhysicalOperator &PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &, PhysicalOperator &) override {
		throw NotImplementedException("rest_ext namespaces are read-only");
	}
	PhysicalOperator &PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &, PhysicalOperator &) override {
		throw NotImplementedException("rest_ext namespaces are read-only");
	}

	DatabaseSize GetDatabaseSize(ClientContext &) override {
		// We're purely in-memory with no real storage, so "0 of everything" is the honest answer.
		return DatabaseSize();
	}
	bool InMemory() override {
		return true;
	}
	string GetDBPath() override {
		return StringUtil::Format("rest_ext namespace (%d resources)", resources.size());
	}

	// This is the key override that makes `gcp.Projects(...)` resolvable at all: it tells DuckDB
	// "if someone qualifies a name with just my catalog name and no schema (like `gcp.Projects`
	// rather than `gcp.main.Projects`), assume they mean the schema called main". Without this,
	// DuckDB would have no way to guess which schema to look in.
	string GetDefaultSchema() const override {
		return "main";
	}

private:
	void DropSchema(ClientContext &, DropInfo &) override {
		throw NotImplementedException("rest_ext namespaces have a single fixed schema; DROP SCHEMA is not "
		                              "supported");
	}

private:
	vector<RestResourceConfig> resources;
	unique_ptr<RestNamespaceSchemaEntry> schema;
};

// See the header comment for why we can't just reuse DuckDB's built-in DuckTransactionManager
// here. There's genuinely nothing for this class to do - no on-disk state to commit, checkpoint,
// or roll back - so every method is either a no-op or the simplest thing that satisfies the
// interface.
class RestTransactionManager : public TransactionManager {
public:
	explicit RestTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override {
		// We still need to hand back a real, alive Transaction object (DuckDB holds onto a
		// reference to it for the lifetime of the query), so we keep a list of them alive here
		// rather than actually doing anything transactional with them.
		auto transaction = make_uniq<Transaction>(*this, context);
		auto &result = *transaction;
		lock_guard<mutex> lock(transaction_lock);
		transactions.push_back(std::move(transaction));
		return result;
	}

	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override {
		return ErrorData(); // "no error" - there's nothing to actually commit
	}

	void RollbackTransaction(Transaction &transaction) override {
		// nothing to roll back
	}

	void Checkpoint(ClientContext &context, bool force) override {
		// nothing to persist
	}

private:
	mutex transaction_lock;
	vector<unique_ptr<Transaction>> transactions;
};

unique_ptr<Catalog> CreateRestNamespaceCatalog(AttachedDatabase &db, vector<RestResourceConfig> resources) {
	auto catalog = make_uniq<RestNamespaceCatalog>(db, std::move(resources));
	catalog->Initialize(false);
	return std::move(catalog);
}

unique_ptr<TransactionManager> CreateRestTransactionManager(AttachedDatabase &db) {
	return make_uniq<RestTransactionManager>(db);
}

} // namespace duckdb
