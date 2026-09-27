# dist-repo

A DuckDB [custom extension repository](https://duckdb.org/docs/extensions/working_with_extensions.html#creating-a-custom-repository),
holding a prebuilt `rest_ext` binary - no separate build step needed to try the extension, just a
clone.

Layout: `<duckdb_version>/<platform>/rest_ext.duckdb_extension.gz`, matching exactly what DuckDB's
`INSTALL ... FROM <repo>` looks up. Currently built for `linux_amd64` only (the platform this was
built on) - other platforms need their own build.

## Usage

Point `custom_extension_repository` at this directory **on your local disk** (after cloning),
not the GitHub URL directly:

```sql
SET custom_extension_repository='/path/to/rest-ext-duckdb/dist-repo';
INSTALL rest_ext;
LOAD rest_ext;
```

Why a local path and not `https://raw.githubusercontent.com/...` directly: fetching a remote
`https://` repository requires the `httpfs` extension to be loaded first, and `httpfs` has no
published binary for this project's exact pinned DuckDB dev commit (an unreleased commit, not a
tagged version) - DuckDB's official extension CDN only publishes for tagged releases. A local path
never goes through that remote-file check at all, so it works regardless.

This binary is also unsigned (not signed with DuckDB's official extension key), so loading it
requires either starting the CLI with `duckdb -unsigned`, or passing `allow_unsigned_extensions=true`
at connection time (it can't be changed with `SET` after the database is already open).

See `../USAGE.md` for what to do with it once loaded.
