# dist-repo

A DuckDB [custom extension repository](https://duckdb.org/docs/extensions/working_with_extensions.html#creating-a-custom-repository),
serving a prebuilt `rest_ext` binary directly via GitHub's raw file hosting - no separate build
step needed to try the extension.

Layout: `<duckdb_version>/<platform>/rest_ext.duckdb_extension.gz`, matching exactly what DuckDB's
`INSTALL ... FROM <repo>` looks up. Currently built for `linux_amd64` only (the platform this was
built on) - other platforms need their own build.

## Usage

```sql
SET custom_extension_repository='https://raw.githubusercontent.com/BrianHardyR/rest-ext-duckdb/main/dist-repo';
INSTALL rest_ext;
LOAD rest_ext;
```

This binary is unsigned (not signed with DuckDB's official extension key), so it also requires
either starting the CLI with `-unsigned`, or:

```sql
SET allow_unsigned_extensions=true;
```

See `../USAGE.md` for what to do with it once loaded.
