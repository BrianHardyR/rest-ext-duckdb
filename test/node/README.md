# Behaviour tests

Drive a built `rest_ext` through `@duckdb/node-api` against a local HTTP server, so pagination,
redirects, header and placeholder handling and `enable_external_access` are checked without
third-party APIs.

```sh
npm install @duckdb/node-api@1.5.5-r.4   # the DuckDB version the extension was built for
REST_EXT=build/release/extension/rest_ext/rest_ext.duckdb_extension node --test test/node/
```

`DUCKDB_NODE_API` may point at a `package.json` whose `node_modules` holds `@duckdb/node-api`.
