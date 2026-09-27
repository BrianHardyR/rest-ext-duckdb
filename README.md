# rest_ext

A DuckDB extension for querying any REST/HTTP API directly from SQL. `ATTACH` an endpoint (or an
OpenAPI spec) and query it like a table - JSON responses are inferred into `STRUCT`/`LIST` columns
automatically.

## Install

Prebuilt binary (Linux x86_64 only, no build required):

```sql
INSTALL httpfs;
LOAD httpfs;
SET custom_extension_repository='https://raw.githubusercontent.com/BrianHardyR/rest-ext-duckdb/main/dist-repo';
INSTALL rest_ext;
LOAD rest_ext;
```

The binary is unsigned, so DuckDB also needs to be started with `duckdb -unsigned` (or
`allow_unsigned_extensions=true` at connection time).

Other platforms - build from source:

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
