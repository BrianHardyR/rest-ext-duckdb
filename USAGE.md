# Using rest_ext

`rest_ext` lets you `ATTACH` a REST API and query it with plain SQL. Each endpoint becomes a table
function that takes a query-params JSON object and a body JSON object, and returns rows with a
schema inferred from the JSON response - nested objects become `STRUCT` columns, arrays become
`LIST` columns.

**Contents:** [How it works](#how-it-works) · [Quick start](#quick-start) ·
[Choosing an ATTACH mode](#choosing-an-attach-mode) · [Single-endpoint](#single-endpoint-mode) ·
[Namespace](#namespace-mode-resources) · [OpenAPI import](#openapi-import-mode-format-openapi) ·
[Calling a resource](#calling-a-resource) · [Response shapes](#response-shape-and-type-inference) ·
[Authentication](#authentication) · [HTTP semantics](#http-semantics) · [DETACH](#detach) ·
[Limitations](#limitations)

## How it works

```
 ATTACH 'url=... / resources={...} / FORMAT openapi' AS alias (TYPE rest_ext)
            │
            ▼
   parses the ATTACH options and registers one callable table function per endpoint
            │
            ▼
 SELECT * FROM alias(query_params, body)   or   alias.Resource(query_params, body)
            │
            ▼
   BIND     — makes the real HTTP request ONCE, then infers output columns/types
            │            from the shape of the JSON response
            ▼
   EXECUTE  — hands back the already-fetched rows, one batch at a time
```

The HTTP call happens at **bind** time (before the query even starts producing rows) - by the time
`EXECUTE` runs, the response is already fetched, parsed, and schema-mapped.

## Quick start

Don't want to build it yourself? Install the prebuilt Linux x86_64 binary directly - no clone, no
compiling:

```sql
INSTALL httpfs;
LOAD httpfs;
SET custom_extension_repository='https://raw.githubusercontent.com/BrianHardyR/rest-ext-duckdb/main/dist-repo';
INSTALL rest_ext;
LOAD rest_ext;
```

A couple of things worth knowing:
- `httpfs` has to be installed *before* setting `custom_extension_repository` - fetching from any
  `https://` repository needs it loaded first, and once `custom_extension_repository` is set, that
  setting also applies to `INSTALL httpfs` itself (so installing it after would send it looking for
  `httpfs` at this repo too, which doesn't have it).
- This binary isn't signed with DuckDB's official key, so it also needs either `duckdb -unsigned`,
  or `allow_unsigned_extensions=true` passed at connection time (it can't be changed with `SET`
  after the database is already open).
- Prebuilt `linux_amd64` binaries are published for DuckDB v1.3.2, v1.4.5, and v1.5.5 - `INSTALL`
  automatically fetches whichever one matches your running DuckDB's exact version (that's how
  `custom_extension_repository` resolution works: DuckDB appends its own version and platform to
  the repository URL). A DuckDB version outside that range, or a non-Linux/non-x86_64 platform,
  needs the build-from-source path below.

Otherwise, build from source:

```sh
make
./build/release/duckdb
```

```sql
INSTALL rest_ext;
LOAD rest_ext;
```

## Choosing an ATTACH mode

| Mode | ATTACH looks like | You call it as | Use it when |
|------|--------------------|-----------------|-------------|
| [Single-endpoint](#single-endpoint-mode) | `url=...` | `alias(params, body)` | one URL is enough |
| [Namespace](#namespace-mode-resources) | `resources={...}` | `alias.Name(params, body)` | a handful of hand-picked endpoints, one alias |
| [OpenAPI import](#openapi-import-mode-format-openapi) | `FORMAT 'openapi'` | `alias.operationId(params, body)` | the API already publishes a spec |

Every callable, in every mode, has the exact same two-argument shape:

```
resource('{"query":"param"}', '{"body":"json"}')
          └──── query_params ────┘  └── body ──┘
```

both required JSON **objects** (as VARCHAR text), even when empty (`'{}'`). See
[Calling a resource](#calling-a-resource) for what each argument does.

Options inside the `(...)` parentheses (`TYPE`, `FORMAT`, `SPEC_URL`, `BASE_URL`, ...) are matched
case-insensitively - `FORMAT`, `Format`, and `format` are all the same option.

## Single-endpoint mode

```sql
ATTACH 'url=<url> headers=<json> method=<GET|POST|PUT|PATCH|DELETE>' AS <alias> (TYPE rest_ext);

SELECT * FROM <alias>(<query_params_json>, <body_json>);
```

- `url=` is the only required piece - omitting it is a bind-time error.
- `headers=` defaults to `{}`, `method=` defaults to `GET`.
- The pieces are found by locating each recognized `key=` marker (`url=`, `headers=`, `method=`,
  `resources=`) in the path text and taking everything up to the next marker as that key's value -
  so a value is free to contain spaces, braces, commas, etc., and the pieces can appear in any
  order.

**Example** - a single JSON object response becomes one row, nested objects/arrays become native
`STRUCT`/`LIST` columns:

```sql
ATTACH 'url=https://archive-api.open-meteo.com/v1/era5 headers={"Accept":"application/json"} method=GET'
    AS weatherapi (TYPE rest_ext);

SELECT latitude, timezone, hourly.time[1] AS first_hour, hourly.temperature_2m[1] AS first_temp
FROM weatherapi(
    '{"latitude":"52.52","longitude":"13.41","start_date":"2021-01-01","end_date":"2021-01-01","hourly":"temperature_2m"}',
    '{}'
);
```
```
┌──────────┬──────────┬──────────────────┬────────────┐
│ latitude │ timezone │    first_hour    │ first_temp │
│  double  │ varchar  │     varchar      │   double   │
├──────────┼──────────┼──────────────────┼────────────┤
│ 52.54833 │ GMT      │ 2021-01-01T00:00 │        0.5 │
└──────────┴──────────┴──────────────────┴────────────┘
```

Its full inferred schema (`DESCRIBE SELECT * FROM weatherapi(...)`):

```
┌───────────────────────────────────────────────────────────────────────┐
│ latitude              double                                          │
│ longitude             double                                          │
│ generationtime_ms     double                                          │
│ utc_offset_seconds    bigint                                          │
│ timezone              varchar                                         │
│ timezone_abbreviation varchar                                         │
│ elevation             double                                          │
│ hourly_units          struct("time" varchar, temperature_2m varchar)  │
│ hourly                struct("time" varchar[], temperature_2m double[])│
└───────────────────────────────────────────────────────────────────────┘
```

A bare, unqualified call like `weatherapi(...)` is looked up by ordinary name resolution (it's
registered in DuckDB's shared system catalog), which never consults what's currently attached -
that's why `DETACH` has to explicitly remove it (see [DETACH](#detach)).

## Namespace mode (`resources=`)

```sql
ATTACH 'resources={"Name1":"url1","Name2":"url2"} headers=<json> method=<METHOD>' AS <alias> (TYPE rest_ext);

SELECT * FROM <alias>.<Name>(<query_params_json>, <body_json>);
```

- `resources=` is a flat JSON object mapping `resource_name -> full URL`. Each resource gets its
  own URL - they don't have to share a host (e.g. mixing several GCP APIs under one alias).
- Every resource in the mapping shares the same `headers=`/`method=` - use
  [OpenAPI import](#openapi-import-mode-format-openapi) if resources need different methods.
- Resource names are looked up case-insensitively (`myapi.Users` == `myapi.users`).
- Backed by a real (in-memory, read-only) `Catalog`+`Schema`, so `DETACH` cleans everything up
  automatically.

**Example:**

```sql
ATTACH 'resources={"Users":"https://jsonplaceholder.typicode.com/users","Posts":"https://jsonplaceholder.typicode.com/posts"}'
    AS jp (TYPE rest_ext);

SELECT (SELECT count(*) FROM jp.Users('{}', '{}')) AS n_users,
       (SELECT count(*) FROM jp.Posts('{}', '{}')) AS n_posts;
```
```
┌─────────┬─────────┐
│ n_users │ n_posts │
│  int64  │  int64  │
├─────────┼─────────┤
│      10 │     100 │
└─────────┴─────────┘
```

## OpenAPI import mode (`FORMAT 'openapi'`)

The easiest way to configure a whole API at once is to point `ATTACH` at its OpenAPI (2.0
"Swagger" or 3.x) spec. Every operation the spec defines becomes its own dot-qualified callable -
same shape as namespace mode, just generated from the spec instead of hand-written.

```sql
-- from a local spec file (the ATTACH path is a filesystem path):
ATTACH '/path/to/spec.json' AS <alias> (TYPE rest_ext, FORMAT 'openapi');

-- from a remote spec (SPEC_URL is fetched with rest_ext's own HTTP client):
ATTACH 'unused' AS <alias> (TYPE rest_ext, FORMAT 'openapi', SPEC_URL 'https://.../openapi.json');

-- overriding whatever base URL the spec itself declares:
ATTACH ... (TYPE rest_ext, FORMAT 'openapi', BASE_URL 'https://api.example.com');
```

**Example** - the official Swagger Petstore demo, attached straight from its live spec:

```sql
ATTACH 'petstore' AS petstore (TYPE rest_ext, FORMAT 'openapi',
    SPEC_URL 'https://petstore3.swagger.io/api/v3/openapi.json');

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

(This is a live, shared public demo, so the exact rows and IDs above will differ if you run this
yourself - the shapes and mechanics won't.)

### Naming and base URL resolution

```
resource name  = operationId, if the spec gives one (e.g. "findPetsByStatus")
                 else sanitized "method_path"  (e.g. "get__pets")
                 else "name_2", "name_3", ...  on a collision

base URL       = BASE_URL option,                      if given (wins over everything below)
                 else spec's OpenAPI 3.x "servers[0]",  {variables} filled from their own defaults
                 else spec's OpenAPI 2.0 host+basePath+schemes
                 else → bind-time error asking for BASE_URL
```

- **Only OpenAPI is supported.** Other formats (e.g. a Postman collection) need converting to
  OpenAPI first, using any of the widely available third-party converters.
- **Relative `servers` URLs** (e.g. just `/api/v3`, meaning "wherever this spec document itself is
  hosted" - a real pattern; the official Petstore v3 spec does exactly this) only resolve when the
  spec was fetched via `SPEC_URL` (its origin is known). A relative URL loaded from a local file
  needs an explicit `BASE_URL`.
- **Why the ATTACH path can't just be an `http://...` URL directly**: DuckDB core intercepts every
  `ATTACH` before this extension ever sees it, and unconditionally requires `httpfs` for any path
  starting with a recognized remote-file prefix - regardless of `TYPE`. `SPEC_URL` sidesteps that
  (only the path itself is checked), so a remote spec never forces you to install `httpfs`. When
  using `SPEC_URL`, the ATTACH path itself is unused - any placeholder string works.
- **Security schemes in the spec are not parsed** - configure auth via
  [`CREATE SECRET`/`headers=`](#authentication) instead.
- Each operation keeps its own HTTP method (`get`/`post`/`put`/`patch`/`delete` - the only methods
  this extension's HTTP client understands; other methods declared in the spec, like `head` or
  `options`, are skipped).

**Example** - `test/fixtures/openapi_sample.json` (checked into this repo) declares:

```json
{
  "openapi": "3.0.0",
  "servers": [{ "url": "https://jsonplaceholder.typicode.com" }],
  "paths": {
    "/users":      { "get": { "operationId": "listUsers" } },
    "/posts":      { "get": { "operationId": "listPosts" } },
    "/users/{id}": { "get": { "operationId": "getUser" } }
  }
}
```

```sql
ATTACH 'test/fixtures/openapi_sample.json' AS petapi (TYPE rest_ext, FORMAT 'openapi');

SELECT name FROM petapi.getUser('{"id":"1"}', '{}');
```
```
┌───────────────┐
│     name      │
│    varchar    │
├───────────────┤
│ Leanne Graham │
└───────────────┘
```

Errors are equally direct. An unrecognized `FORMAT`:

```sql
ATTACH 'test/fixtures/openapi_sample.json' AS x (TYPE rest_ext, FORMAT 'postman');
```
```
Binder Error: rest_ext: unrecognized FORMAT "postman" (expected 'openapi')
```

`BASE_URL` overriding the spec's own `servers` entry (confirmed here by the overridden host
showing up in the resulting request, since this host has no `/users` endpoint to actually succeed
against):

```sql
ATTACH 'test/fixtures/openapi_sample.json' AS x (TYPE rest_ext, FORMAT 'openapi', BASE_URL 'https://postman-echo.com');
SELECT * FROM x.listUsers('{}', '{}');
```
```
IO Error: rest_ext: HTTP request to "https://postman-echo.com/users" returned status 404: ...
```

## Calling a resource

Every callable resource - however it was ATTACHed - has the same signature:

```sql
resource(query_params_json, body_json)
```

### Path parameters vs. the query string

Each key in `query_params_json` is routed one of two ways:

```
query_params = {"id": "1", "sort": "asc"}
URL template = https://api.example.com/users/{id}
                                              │        │
                        ┌─────────────────────┘        └───────────────┐
                        ▼                                               ▼
          "{id}" found in the URL →                     no matching placeholder →
          substituted into the path,                    appended as "?sort=asc"
          NOT sent again as a query param
```

```sql
-- URL template: https://jsonplaceholder.typicode.com/users/{id}
SELECT name FROM petapi.getUser('{"id":"1"}', '{}');
```
```
┌───────────────┐
│     name      │
│    varchar    │
├───────────────┤
│ Leanne Graham │
└───────────────┘
```

A placeholder with no matching key is a clear, immediate bind error naming exactly which `{name}`
is missing, rather than silently sending a broken URL:

```sql
SELECT * FROM petapi.getUser('{}', '{}');
```
```
Invalid Input Error: rest_ext: URL "https://jsonplaceholder.typicode.com/users/{id}" requires a
value for "{id}" - pass it as a key in the query_params argument, e.g. '{"id": "..."}'
```

### Body and headers

`body_json` is sent as the request body for `POST`/`PUT`/`PATCH`/`DELETE`, and ignored for `GET`.
`Content-Type` gets special handling: if present among the resolved headers its value is used as
the request's content type; otherwise it defaults to `application/json`. Either way it's only ever
sent once - never duplicated.

## Response shape and type inference

```
JSON response
   │
   ├─ array            → one row per element, columns = union of every element's keys
   │                      mismatched types across rows are WIDENED, not errored
   │
   ├─ object           → exactly one row, top-level keys become columns
   │
   ├─ bare scalar, or   → no natural column name → wrapped as one column called "value"
   │  array of scalars
   │
   └─ not JSON (or      → falls back to one column called "result" holding the raw text
      empty body)          rather than erroring
```

**Type widening across array rows** - a whole number and a decimal number for the same key widen
to `DOUBLE`:

```sql
-- response: [{"id": 1, "score": 5}, {"id": 2, "score": 5.5}]
SELECT typeof(score), id, score FROM widen('{}', '{}');
```
```
┌───────────────┬───────┬────────┐
│ typeof(score) │  id   │ score  │
│    varchar    │ int64 │ double │
├───────────────┼───────┼────────┤
│ DOUBLE        │     1 │    5.0 │
│ DOUBLE        │     2 │    5.5 │
└───────────────┴───────┴────────┘
```

**Struct merging** - two `STRUCT`s merge into the union of both sets of fields; a field missing
from one row's JSON simply becomes `NULL`:

```sql
-- response: [{"id": 1, "meta": {"a": 1}}, {"id": 2, "meta": {"b": 2}}]
SELECT id, meta.a, meta.b FROM merged('{}', '{}');
```
```
┌───────┬───────┬───────┐
│  id   │   a   │   b   │
│ int64 │ int64 │ int64 │
├───────┼───────┼───────┤
│     1 │     1 │  NULL │
│     2 │  NULL │     2 │
└───────┴───────┴───────┘
```

(`LIST`s merge the same way, by merging their element types; anything that genuinely clashes - e.g.
one row has a number where another has a whole object for the same key - falls back to `VARCHAR`,
with the JSON re-serialized to text for values that aren't naturally strings, so one unusual row
doesn't blow up the whole query. An empty array has nothing to infer from, so it defaults to a
single `VARCHAR` column called `value`.)

**Scalar wrapping** - a bare array of non-objects has no field names to use as columns:

```sql
-- response: [1, 2, 3]
SELECT * FROM scalars('{}', '{}');
```
```
┌───────┐
│ value │
│ int64 │
├───────┤
│     1 │
│     2 │
│     3 │
└───────┘
```

**Non-JSON fallback** - e.g. plain HTML - is not an error:

```sql
ATTACH 'url=https://example.com/ method=GET' AS htmlpage (TYPE rest_ext);
SELECT result LIKE '%<html%' AS looks_like_html, length(result) AS body_length
FROM htmlpage('{}', '{}');
```
```
┌─────────────────┬─────────────┐
│ looks_like_html │ body_length │
│     boolean     │    int64    │
├─────────────────┼─────────────┤
│ true            │         559 │
└─────────────────┴─────────────┘
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

```
resolved headers for a request to https://api.example.com/...
   1. start from the best-matching secret's HEADERS (longest SCOPE-prefix wins)
   2. layer the ATTACH string's own headers=... on top
   3. a key present in BOTH → the inline ATTACH value wins
```

- `rest_ext_headers` is a generic header bag - not tied to any one auth scheme, so it works
  equally well for a bearer token, an API key header, pre-encoded Basic auth, or anything else a
  REST API wants in its headers.
- `SCOPE` matching is longest-prefix, the same mechanism DuckDB's built-in `http`/S3 secrets use.
- Header values are marked sensitive: `duckdb_secrets()` shows `headers=redacted`, never the real
  values.
- Resolved once, at `ATTACH` time, independently **per resource URL** - a namespace or OpenAPI
  import spanning multiple hosts can match a different secret for each one.

**Example** - a secret provides a default, an inline ATTACH header overrides just one key:

```sql
CREATE SECRET rest_ext_doc_secret (
    TYPE rest_ext_headers,
    HEADERS MAP {'X-From-Secret': 'secret-value', 'X-Override-Me': 'from-secret'},
    SCOPE 'https://postman-echo.com'
);

ATTACH 'url=https://postman-echo.com/get headers={"X-Override-Me":"from-inline"} method=GET'
    AS secretapi (TYPE rest_ext);

SELECT headers['x-override-me'] AS override_me, headers['x-from-secret'] AS from_secret
FROM secretapi('{}', '{}');
```
```
┌─────────────┬──────────────┐
│ override_me │ from_secret  │
│   varchar   │   varchar    │
├─────────────┼──────────────┤
│ from-inline │ secret-value │
└─────────────┴──────────────┘
```

## HTTP semantics

| | |
|---|---|
| Methods | `GET`, `POST`, `PUT`, `PATCH`, `DELETE` (case-insensitive); anything else is a clear error |
| Connect timeout | 10s |
| Read timeout | 30s |
| Write timeout | 30s |
| Redirects | followed automatically |
| Non-2xx status | raises `IOException`, including the response status/body |
| Network failure | raises `IOException` with the underlying error (DNS, TLS, connection refused, timeout, ...) |

## DETACH

- **Single-endpoint mode**: `DETACH <alias>` removes the callable registered under that name in
  DuckDB's shared system catalog. Without this, the function would keep answering to its old name
  forever, and re-`ATTACH`ing the same alias would fail with "already exists".
- **Namespace and OpenAPI import mode**: nothing special needed - a namespace's resources live
  entirely inside its own attachment, so `DETACH` cleans everything up automatically, and
  re-`ATTACH`ing the same alias (with the same or a different resource set) just works.

## Limitations

- OpenAPI (2.0/3.x) is the only supported spec format - convert other formats (e.g. Postman) to
  OpenAPI first.
- One HTTP call per table-function invocation, so no built-in pagination: all output rows come from
  that single response. An API that returns results a page at a time (e.g. a
  `next_cursor`/`next_page_token` field, or a `Link` header) only gets you that one page per call -
  `rest_ext` doesn't follow cursors or walk subsequent pages automatically. Paging through results
  means issuing repeat calls yourself, feeding each response's cursor value back in as the next
  call's query param (e.g. via a recursive CTE, or a loop in whatever's driving the SQL).
- OpenAPI security schemes aren't parsed - configure auth via `CREATE SECRET`/`headers=` instead.
- Namespace mode's hand-written `resources={...}` gives every resource the same HTTP method; use
  OpenAPI import if you need a mix of GET/POST/etc. under one alias.
- Prebuilt binaries are only published for `linux_amd64` on DuckDB v1.3.2, v1.4.5, and v1.5.5 (see
  [Quick start](#quick-start)) - other platforms, or a DuckDB release outside that range, need
  building from source. DuckDB's extension C++ API isn't ABI-stable across releases, so a DuckDB
  version further outside the tested range may hit a not-yet-handled internal API break when
  building from source (see `CMakeLists.txt` and `src/include/rest_ext_compat.hpp` for the
  version-detection pattern already used to bridge v1.3.x-v1.5.x).

## Further examples

See [test/sql/rest_ext.test](test/sql/rest_ext.test) (all three ATTACH modes, secrets, path
parameters, error cases) and
[test/sql/rest_ext_openapi_live.test_slow](test/sql/rest_ext_openapi_live.test_slow) (a live
OpenAPI walkthrough against the Swagger Petstore demo).
