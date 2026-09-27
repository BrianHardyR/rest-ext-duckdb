# dist-repo

A DuckDB [custom extension repository](https://duckdb.org/docs/extensions/working_with_extensions.html#creating-a-custom-repository),
serving a prebuilt `rest_ext` binary directly via GitHub's raw file hosting - no build step, no
clone, just point DuckDB at it.

Layout: `<duckdb_version>/<platform>/rest_ext.duckdb_extension.gz`, matching exactly what DuckDB's
`INSTALL ... FROM <repo>` looks up. Built against DuckDB **v1.5.5** (a real tagged release - this
matters, see below), `linux_amd64` only; other platforms need their own build.

## Usage

```sql
INSTALL httpfs;
LOAD httpfs;
SET custom_extension_repository='https://raw.githubusercontent.com/BrianHardyR/rest-ext-duckdb/main/dist-repo';
INSTALL rest_ext;
LOAD rest_ext;
```

Install `httpfs` **before** setting `custom_extension_repository`: fetching anything from a
`https://` repository requires `httpfs` loaded first (DuckDB core's own remote-file check), and
once `custom_extension_repository` is set, it also redirects `INSTALL httpfs` itself to look here
- where there is no `httpfs` binary. Installing `httpfs` from the default repository first avoids
that chicken-and-egg problem entirely.

This only works at all because the extension is built against a real, tagged DuckDB release
(v1.5.5): DuckDB's official CDN only publishes extensions like `httpfs` for tagged releases, never
for arbitrary unreleased commits - so `httpfs` genuinely has a matching binary to fetch here.

This binary is also unsigned (not signed with DuckDB's official extension key), so loading it
requires either starting the CLI with `duckdb -unsigned`, or passing `allow_unsigned_extensions=true`
at connection time (it can't be changed with `SET` after the database is already open).

See `../USAGE.md` for what to do with it once loaded.
