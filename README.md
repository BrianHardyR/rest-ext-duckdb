# rest_ext

A DuckDB extension for querying any REST/HTTP API directly from SQL. `ATTACH` an endpoint (or an
OpenAPI spec) and query it like a table - JSON responses are inferred into `STRUCT`/`LIST` columns
automatically.

## Install

Prebuilt binary (Linux x86_64, DuckDB v1.3.2/v1.4.5/v1.5.5 - no build required):

```sql
INSTALL httpfs;
LOAD httpfs;
SET custom_extension_repository='https://raw.githubusercontent.com/BrianHardyR/rest-ext-duckdb/main/dist-repo';
INSTALL rest_ext;
LOAD rest_ext;
```

`INSTALL` automatically fetches the binary matching your running DuckDB's exact version. The
binary is unsigned, so DuckDB also needs to be started with `duckdb -unsigned` (or
`allow_unsigned_extensions=true` at connection time).

Other platforms, or a DuckDB version outside that range - build from source:

```sh
git clone --recurse-submodules https://github.com/BrianHardyR/rest-ext-duckdb.git
cd rest-ext-duckdb
make
./build/release/duckdb
```

```sql
INSTALL rest_ext;
LOAD rest_ext;
```

## Use

Point `ATTACH` at an OpenAPI spec to turn every operation it defines into a callable function:

```sql
ATTACH 'petstore' AS petstore (TYPE rest_ext, FORMAT 'openapi',
    SPEC_URL 'https://petstore3.swagger.io/api/v3/openapi.json');

SELECT id, name, status
FROM petstore.findPetsByStatus('{"status":"available"}', '{}')
LIMIT 5;
```

No spec? Attach a single endpoint, or several named endpoints, directly:

```sql
-- One endpoint:
ATTACH 'url=https://api.example.com/users method=GET' AS myapi (TYPE rest_ext);
SELECT * FROM myapi('{}', '{}');

-- Several named endpoints under one alias:
ATTACH 'resources={"Users":"https://api.example.com/users","Orders":"https://api.example.com/orders"}'
    AS myapi (TYPE rest_ext);
SELECT * FROM myapi.Users('{}', '{}');
```

Every function takes a query-params JSON object and a body JSON object. Path placeholders (e.g.
`/pet/{petId}`) are filled from the same query-params object - no separate syntax needed.

Auth headers can be stored once as a secret and matched to a target URL automatically:

```sql
CREATE SECRET (
    TYPE rest_ext_headers,
    HEADERS MAP {'Authorization': 'Bearer <token>'},
    SCOPE 'https://api.example.com'
);
```

See [USAGE.md](USAGE.md) for the full walkthrough, and `test/sql/rest_ext.test` /
`test/sql/rest_ext_openapi_live.test_slow` for more worked examples.

## Supported DuckDB versions

| | |
|---|---|
| Prebuilt binaries | `linux_amd64` only, for DuckDB **v1.3.2, v1.4.5, and v1.5.5** - `INSTALL` fetches whichever matches your running DuckDB automatically |
| Build from source | any other DuckDB version/platform - see [Cross-version compatibility](#cross-version-compatibility) below |

## Limitations

- OpenAPI (2.0/3.x) is the only supported spec format - convert other formats (e.g. Postman) to
  OpenAPI first.
- One HTTP call per table-function invocation, so no built-in pagination: an API that returns
  results a page at a time (e.g. a `next_cursor`/`next_page_token` field, or a `Link` header) only
  gets you that one page per call - `rest_ext` doesn't follow cursors or walk subsequent pages
  automatically. Paging through results means issuing repeat calls yourself (e.g. a recursive CTE
  or a loop in application code) with each page's cursor value passed back in as a query param.
- OpenAPI security schemes aren't parsed - configure auth via `CREATE SECRET`/`headers=` instead.
- Namespace mode's hand-written `resources={...}` gives every resource the same HTTP method; use
  OpenAPI import for a mix of GET/POST/etc.
- Prebuilt binaries only cover `linux_amd64` on the three DuckDB versions above; a DuckDB release
  outside that range may hit a not-yet-handled internal API change when built from source (DuckDB's
  extension C++ API isn't ABI-stable across releases - see below).

See [USAGE.md](USAGE.md#limitations) for the full list with more detail.

## Development

Built on the [DuckDB extension template](https://github.com/duckdb/extension-template).

```sh
make          # build ./build/release/duckdb with the extension loaded
make test     # run the SQL tests in test/sql
```

Submodules: `duckdb` (core, pinned to v1.5.5) and `extension-ci-tools` (build/test/deploy
tooling). Update with:

```sh
git submodule update --init --recursive
```

### Cross-version compatibility

The source targets a range of DuckDB core versions (currently v1.3.2 through v1.5.5) from one
codebase, not just the pinned submodule version - DuckDB's own extension C++ API isn't ABI-stable
across releases, so a handful of call sites (`StorageExtension` registration, the extension entry
point, and the `ATTACH` callback's options parameter) are guarded by compile-time detection instead
of hardcoded to one version's API - see the comments in `CMakeLists.txt` and
`src/include/rest_ext_compat.hpp`. `dist-repo/` holds one prebuilt binary per supported version,
built by checking out `duckdb`/`extension-ci-tools` to each target tag in turn and rebuilding -
there's no single binary that works everywhere (DuckDB's storage-extension/`ATTACH`/secret-type
APIs aren't exposed through DuckDB's stable C API at all yet, only the classic internal C++ one).
Adding support for a new DuckDB release means bumping the submodules to that tag, fixing whatever
new API break turns up the same way, rebuilding, and dropping the binary into
`dist-repo/v<version>/linux_amd64/`.
