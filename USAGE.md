# Using rest_ext

`rest_ext` lets you `ATTACH` a REST API and query it with plain SQL. Each endpoint becomes a table
function that takes a query-params JSON object and a body JSON object, and returns rows with a
schema inferred from the JSON response - nested objects become `STRUCT` columns, arrays become
`LIST` columns.

**Contents:** [How it works](#how-it-works) · [Quick start](#quick-start) ·
[Choosing an ATTACH mode](#choosing-an-attach-mode) · [Single-endpoint](#single-endpoint-mode) ·
[Namespace](#namespace-mode-resources) · [OpenAPI import](#openapi-import-mode-format-openapi) ·
[Calling a resource](#calling-a-resource) · [Response shapes](#response-shape-and-type-inference) ·
[Pagination](#pagination) · [Authentication](#authentication) · [HTTP semantics](#http-semantics) ·
[Locking a database](#locking-a-database) · [DETACH](#detach) ·
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

## Pagination

Endpoints that return results a page at a time are walked with ATTACH options. Each takes an
RFC 6901 JSON pointer into one page's response:

| Option | Meaning |
|---|---|
| `ITEMS '/data'` | the array of rows in each page (default: the whole response) |
| `NEXT_URL '/nextLink'` | a full URL for the next page; requested with the same method, headers and body |
| `NEXT_TOKEN '/cursor'` | an opaque cursor for the next page, sent back through one of: |
| `TOKEN_BODY '/options/cursor'` | ...written into the request body at this pointer |
| `TOKEN_PARAM 'cursor'` | ...added as this query-string parameter |
| `PAGE_PARAM 'page[number]'` | a page number, sent as this query-string parameter and counted up by one per page... |
| `PAGE_START 1` | ...from this number (default 0) |
| `OFFSET_PARAM 'start'` | how many rows were read before this page, sent as this query-string parameter |
| `PAGE_SIZE 1000` | with `PAGE_PARAM` or `OFFSET_PARAM`: a page with fewer rows than this is the last one; without it, only an empty page is |
| `MAX_PAGES 10000` | stop with an error after this many requests (default 10000) |
| `COLUMNS '/columns'` | for a tabular response (`{"columns": [...], "rows": [[...], ...]}`): name each row array's values after this column list, whose entries are names or objects with a `"name"` |
| `COLUMN_TYPES '{"Cost":"DOUBLE"}'` | declare a column's type (`VARCHAR`, `BIGINT`, `DOUBLE`, `BOOLEAN`, or a `LIST` or `STRUCT` of those) instead of inferring it; declared columns are present even when there are no rows |

These options also work on an endpoint that doesn't paginate. Set one of `NEXT_URL`,
`NEXT_LINK_HEADER`, `NEXT_TOKEN`, `PAGE_PARAM` and `OFFSET_PARAM`: they are five ways of asking for the
next page. `NEXT_LINK_HEADER` and `MAX_ROWS` are described under
[Retries, bounds and call diagnostics](#retries-bounds-and-call-diagnostics).

Paging stops when the cursor is missing, null or empty, or - for `PAGE_PARAM` and `OFFSET_PARAM`,
which have no cursor to read - when a page is empty or shorter than `PAGE_SIZE`. The page size
itself is an ordinary query parameter the caller sends (`'{"count":"1000"}'`); `PAGE_SIZE` only
tells rest_ext what a full page is, so it can stop without asking for an empty one.

The scan streams: the column schema is inferred from the first page that has rows, later pages are
fetched only as the query reaches them (so `LIMIT 10` makes one request, not a hundred), and a row
on a later page that doesn't fit that schema is an error rather than a silent `NULL` - declare a
column with `COLUMN_TYPES` when a first page could mislead inference (a cost of `0` infers
`BIGINT`; `1.25` on page two then fails). An interrupted query stops between pages.

```sql
-- A next-page link (Azure Cost Management):
ATTACH 'url=https://management.azure.com/subscriptions/<id>/providers/Microsoft.CostManagement/query?api-version=2023-11-01 method=POST headers={"Authorization":"Bearer <token>"}'
    AS costs (TYPE rest_ext, ITEMS '/properties/rows', COLUMNS '/properties/columns',
              NEXT_URL '/properties/nextLink', COLUMN_TYPES '{"Cost":"DOUBLE"}');

-- A cursor in the body (Azure Resource Graph):
ATTACH 'url=https://management.azure.com/providers/Microsoft.ResourceGraph/resources?api-version=2022-10-01 method=POST headers={"Authorization":"Bearer <token>"}'
    AS graph (TYPE rest_ext, ITEMS '/data', NEXT_TOKEN '/$skipToken', TOKEN_BODY '/options/$skipToken');
```

```sql
-- An offset (Datadog's host list, 1000 hosts a page):
ATTACH 'url=https://api.datadoghq.com/api/v1/hosts method=GET headers={"DD-API-KEY":"<key>","DD-APPLICATION-KEY":"<key>"}'
    AS hosts (TYPE rest_ext, ITEMS '/host_list', OFFSET_PARAM 'start', PAGE_SIZE 1000);
SELECT host_name FROM hosts('{"count":"1000"}', '{}');

-- A page number counted from 0 (Datadog's users):
ATTACH 'url=https://api.datadoghq.com/api/v2/users method=GET headers={...}'
    AS users (TYPE rest_ext, ITEMS '/data', PAGE_PARAM 'page[number]', PAGE_SIZE 100);
SELECT id, attributes.email FROM users('{"page[size]":"100"}', '{}');
```

A `NEXT_URL` (or `NEXT_LINK_HEADER` target) is only followed on the endpoint's own origin (scheme, host
and port), so a response can't redirect the request's headers - its credentials - to another host.

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
- `SCOPE` matching is longest-prefix, but only up to a URL boundary: a scope matches the URL itself,
  or a prefix of it followed by `/`, `?` or `#` (or a scope ending in `/`). So a scope of
  `https://api.example.com` applies to `https://api.example.com/v1/x` but not to
  `https://api.example.com.evil.net` or `https://api.example.com:8443`.
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

## Retries, bounds and call diagnostics

Every option here is optional and per `ATTACH`; with none of them a call is one attempt with the
default timeouts, exactly as before (the one visible difference is the `User-Agent`, below). They work
in single-endpoint, namespace and `FORMAT 'openapi'` mode alike (for the last, they apply to the spec
fetch and every operation).

| Option | Meaning |
|---|---|
| `RETRIES 3` | extra attempts after a retryable failure (default 0, at most 20) |
| `RETRY_BACKOFF_MS 500` | base of the wait between attempts (default 500): the n-th wait is uniform in `[d/2, d]` with `d = min(cap, base * 2^n)` |
| `RETRY_CAP_MS 30000` | the longest single wait, `Retry-After` included (default 30000; not below `RETRY_BACKOFF_MS`) |
| `TIMEOUT_MS 5000` | connect, read and write timeout per attempt (default: 10s connect, 30s read/write) |
| `MAX_BYTES 1000000` | refuse a response body larger than this; reading stops as soon as it is exceeded |
| `MAX_ROWS 5000` | stop returning rows after this many, without an error; paging stops with it |
| `NEXT_LINK_HEADER true` | the next page is the target of the `Link` response header's `rel="next"` (absolute or relative); same origin only, like `NEXT_URL`; `ITEMS` is optional |
| `RATE_LIMIT_HEADERS '["x-ratelimit-remaining","ratelimit-*"]'` | which response headers to record in `rest_ext_calls()` (case-insensitive; a trailing `*` is a prefix). Default: `Retry-After`, `X-RateLimit-*`, `RateLimit-*`, `x-ms-ratelimit-remaining-*`. `'[]'` records none |
| `USER_AGENT 'my-app/1.0'` | the `User-Agent` request header (default `rest_ext`); a `User-Agent` in `headers=` or a secret wins |

**What is retried.** Status 429, 502, 503 and 504, and a connection that fails, resets or times out.
Never any other status (404 and 500 fail on the first response). A `Retry-After` header (seconds or an
HTTP date) sets the wait, cut to `RETRY_CAP_MS`; without one the jittered exponential backoff applies.
The retry is of the whole request, body included, so a `POST` that is not safe to repeat should not
set `RETRIES`. A body that dies half way is a failed attempt and is retried too.

```sql
ATTACH 'url=https://api.example.com/v1/things/{id}' AS api
    (TYPE rest_ext, RETRIES 3, RETRY_BACKOFF_MS 100, RETRY_CAP_MS 5000);
SELECT * FROM api('{"id":"orders"}', '{}');   -- the server answers 429 Retry-After: 1 twice, then 200
```

```text
┌───────┬────────────────────────────┬────────┬─────────┬─────────┬───────────┬────────────────┬────────────────────────────────┐
│ alias │          endpoint          │ status │ outcome │ retries │ waited_ms │ retry_after_ms │           rate_limit           │
├───────┼────────────────────────────┼────────┼─────────┼─────────┼───────────┼────────────────┼────────────────────────────────┤
│ api   │ 127.0.0.1:45909/flaky/{id} │ 200    │ ok      │ 2       │ 2000      │ 1000           │ {"x-ratelimit-remaining":"99"} │
└───────┴────────────────────────────┴────────┴─────────┴─────────┴───────────┴────────────────┴────────────────────────────────┘
```
(`SELECT alias, endpoint, status, outcome, retries, waited_ms, retry_after_ms, rate_limit FROM rest_ext_calls()`.)

### `rest_ext_calls()`

A table function over the last 500 HTTP calls made by this DuckDB instance (oldest first, any
`ATTACH`, thread-safe, in memory only). One row is one call, retries and same-origin redirects
included; a paged read is one row per page. `rest_ext_calls_clear()` empties it and returns
`cleared BIGINT`, the number of rows removed.

| Column | Type | |
|---|---|---|
| `alias` | VARCHAR | the `ATTACH` name |
| `endpoint` | VARCHAR | host (and port) plus the path *template*: `127.0.0.1:45909/flaky/{id}`. No scheme, credentials, query string or path-parameter values |
| `method` | VARCHAR | `GET`, `POST`, ... |
| `page_index` | INTEGER | 0 for the first request of a scan, then 1, 2, ... |
| `started_at` | TIMESTAMP | UTC, when the call began |
| `latency_ms` | BIGINT | the whole call: every attempt plus every wait |
| `status` | INTEGER | the final HTTP status; NULL if there was no response |
| `outcome` | VARCHAR | `ok`, `http_error`, `network`, `timeout`, `rate_limited` (429, or 503 with `Retry-After`), `bound_hit`, `invalid_body` |
| `bytes` | BIGINT | response body bytes read |
| `rows` | BIGINT | rows in the response; NULL if it failed |
| `retries` | INTEGER | extra attempts made |
| `waited_ms` | BIGINT | time slept between attempts |
| `retry_after_ms` | BIGINT | the `Retry-After` the server last sent, as a wait; NULL if none (it can exceed `waited_ms`: the wait is cut to `RETRY_CAP_MS`) |
| `rate_limit` | VARCHAR | JSON object of the recorded response headers (names lower-cased), from the final response; NULL if none |
| `error` | VARCHAR | what went wrong, short; NULL for `ok` |

`bound_hit` marks a call that was stopped by `MAX_BYTES` (an error, the body is too big to use), or the
last call of a scan that stopped at `MAX_ROWS` or `MAX_PAGES` while the API may have had more. Only the
endpoint template, the status and the headers asked for are recorded: never a header, credential or
query value that was sent. The `error` text for a non-2xx response carries the first 200 characters of
the body with any header or query value of the request masked as `***`. A secret placed in the URL
*path* of the template itself would be visible, so keep credentials in headers or query parameters.

```sql
ATTACH 'url=https://api.example.com/v1/hosts' AS hosts
    (TYPE rest_ext, ITEMS '/data', NEXT_LINK_HEADER true, MAX_ROWS 7);
SELECT count(*) FROM hosts('{}', '{}');                                   -- 7, with no error
SELECT page_index, rows, outcome, error FROM rest_ext_calls() ORDER BY page_index;
```

```text
┌────────────┬──────┬───────────┬─────────────────────────┐
│ page_index │ rows │  outcome  │          error          │
├────────────┼──────┼───────────┼─────────────────────────┤
│ 0          │ 3    │ ok        │ NULL                    │
│ 1          │ 3    │ ok        │ NULL                    │
│ 2          │ 3    │ bound_hit │ stopped at MAX_ROWS (7) │
└────────────┴──────┴───────────┴─────────────────────────┘
```

```sql
ATTACH 'url=https://api.example.com/v1/slow/{id}' AS slowapi (TYPE rest_ext, TIMEOUT_MS 300);
SELECT * FROM slowapi('{"id":"s"}', '{}');     -- IO Error: ... failed: Failed to read connection
SELECT outcome, status, error FROM rest_ext_calls();   -- timeout | NULL | timed out: Failed to read connection

ATTACH 'url=https://api.example.com/v1/big' AS bigapi (TYPE rest_ext, MAX_BYTES 10000);
SELECT count(*) FROM bigapi('{}', '{}');       -- IO Error: ... larger than MAX_BYTES (10000); not reading it
SELECT outcome, status, error FROM rest_ext_calls();   -- bound_hit | 200 | response is larger than MAX_BYTES (10000)

ATTACH 'url=https://api.example.com/v1/quota' AS rl
    (TYPE rest_ext, RATE_LIMIT_HEADERS '["x-ratelimit-remaining","x-custom-quota"]', USER_AGENT 'dashboard/2.0');
SELECT rate_limit FROM rest_ext_calls();       -- {"x-ratelimit-remaining":"7","x-custom-quota":"5"}
```

`timeout` is a connect timeout, or a read or write that ran out its `TIMEOUT_MS` (a stalled body counts);
httplib reports the latter as a read error, so a read that fails after most of the timeout is classed as
a timeout. The timeout applies to each read, so a body that trickles in slowly is not cut off as a whole.
The default `User-Agent` is `rest_ext`; before it was httplib's own.

## HTTP semantics

| | |
|---|---|
| Methods | `GET`, `POST`, `PUT`, `PATCH`, `DELETE` (case-insensitive); anything else is a clear error |
| Connect timeout | 10s (`TIMEOUT_MS` overrides) |
| Read timeout | 30s (`TIMEOUT_MS` overrides) |
| Write timeout | 30s (`TIMEOUT_MS` overrides) |
| Retries | none unless `RETRIES` is set; see [Retries, bounds and call diagnostics](#retries-bounds-and-call-diagnostics) |
| Redirects | followed on the same origin only (up to 5); a redirect to another origin is an error, so headers are never forwarded to another host |
| Headers | a name must be an HTTP token and a value may not contain a control character (CR/LF would inject headers) |
| Path `{placeholders}` | values are percent-encoded (`/` kept); a `..` segment is refused; a placeholder in the host may only hold letters, digits, `-` and `.` |
| Non-2xx status | raises `IOException`, including the response status/body |
| Network failure | raises `IOException` with the underlying error (DNS, TLS, connection refused, timeout, ...) |

## Locking a database

With `enable_external_access = false`, `ATTACH ... (TYPE rest_ext)` is refused, while endpoints
attached before the setting was turned off keep working. A host can attach the endpoints it allows,
then lock the database so the SQL it runs can't reach anywhere else:

```sql
ATTACH 'url=https://api.example.com/v1/items' AS items (TYPE rest_ext);
SET enable_external_access = false;
SET lock_configuration = true;
SELECT * FROM items('{}', '{}');                               -- works
ATTACH 'url=https://elsewhere.example' AS x (TYPE rest_ext);  -- Permission Error
```

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
- Pagination (see [Pagination](#pagination)) follows a cursor or next-page link in the JSON body, or
  a `Link` header with `NEXT_LINK_HEADER`. The pagination options don't apply to `FORMAT 'openapi'`.
- A single request can block for up to the read timeout (30s, or `TIMEOUT_MS`); an interrupt takes
  effect between pages and during a retry wait, not inside one request.
- `rest_ext_calls()` is per DuckDB instance and in memory: it is empty after a restart and holds 500 calls.
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
