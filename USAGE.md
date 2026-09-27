# Using rest_ext

`rest_ext` lets you `ATTACH` a REST API and query it with plain SQL. Each endpoint becomes a table
function that takes a query-params JSON object and a body JSON object, and returns rows with a
schema inferred from the JSON response - nested objects become `STRUCT` columns, arrays become
`LIST` columns.

## Quick start

```sh
make
./build/release/duckdb
```

```sql
INSTALL rest_ext;
LOAD rest_ext;
```

## Example: the Swagger Petstore API

The easiest way to configure a whole API at once is to point `ATTACH` at its OpenAPI spec. The
official Petstore demo publishes one, so no local file is needed - `SPEC_URL` fetches it directly:

```sql
ATTACH 'petstore' AS petstore (TYPE rest_ext, FORMAT 'openapi',
    SPEC_URL 'https://petstore3.swagger.io/api/v3/openapi.json');
```

This reads the spec, and turns every operation it defines (`findPetsByStatus`, `getPetById`,
`addPet`, ...) into a callable `petstore.<operationId>(query_params, body)` function - each already
pointed at the right URL, method, and path.

Query params go in the first argument, as a JSON object:

```sql
SELECT id, name, status
FROM petstore.findPetsByStatus('{"status":"available"}', '{}')
LIMIT 5;
```

```
┌───────────────┬───────────────────┬───────────┐
│      id       │       name        │  status   │
│     int64     │      varchar      │  varchar  │
├───────────────┼───────────────────┼───────────┤
│      -5216996 │ B@Þ               │ available │
│ 1790355770971 │ Rex-1790355770971 │ available │
│ 1790355837460 │ Rex-1790355837460 │ available │
│ 1790355839043 │ Rex-1790355839043 │ available │
│ 1790356921263 │ Rex               │ available │
└───────────────┴───────────────────┴───────────┘
```

A path like `/pet/{petId}` takes its `{petId}` straight out of the same query-params object -
no separate placeholder syntax needed:

```sql
SELECT id, name, status
FROM petstore.getPetById('{"petId":"1790356921263"}', '{}');
```

```
┌───────────────┬─────────┬───────────┐
│      id       │  name   │  status   │
│     int64     │ varchar │  varchar  │
├───────────────┼─────────┼───────────┤
│ 1790356921263 │ Rex     │ available │
└───────────────┴─────────┴───────────┘
```

(This is a live, shared public demo, so the exact rows and IDs above will differ if you run this
yourself - the shapes and mechanics won't.)

## Other ways to configure an API

Don't have (or need) a spec? Two lighter-weight options:

```sql
-- One endpoint, no spec at all:
ATTACH 'url=https://api.example.com/users method=GET' AS myapi (TYPE rest_ext);
SELECT * FROM myapi('{}', '{}');

-- Several named endpoints under one alias:
ATTACH 'resources={"Users":"https://api.example.com/users","Orders":"https://api.example.com/orders"}'
    AS myapi (TYPE rest_ext);
SELECT * FROM myapi.Users('{}', '{}');
```

## Authentication

Headers (API keys, bearer tokens, etc.) can be passed inline, or stored once as a secret and
matched to a target URL automatically:

```sql
CREATE SECRET (
    TYPE rest_ext_headers,
    HEADERS MAP {'Authorization': 'Bearer <token>'},
    SCOPE 'https://api.example.com'
);
```

Any request to a matching URL picks the secret's headers up automatically - no need to repeat them
in every `ATTACH`.

See `test/sql/rest_ext.test` and `test/sql/rest_ext_openapi_live.test_slow` for further worked
examples.
