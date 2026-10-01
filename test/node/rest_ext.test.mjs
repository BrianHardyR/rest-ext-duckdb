// Behaviour of a built rest_ext against a local HTTP server, through @duckdb/node-api.
// Run: REST_EXT=path/to/rest_ext.duckdb_extension node --test test/node/
import { strict as assert } from 'node:assert';
import { createServer } from 'node:http';
import { after, before, describe, it } from 'node:test';
import { createRequire } from 'node:module';

const require = createRequire(process.env.DUCKDB_NODE_API ?? import.meta.url);
const { DuckDBInstance } = require('@duckdb/node-api');
const EXTENSION = process.env.REST_EXT;
if (!EXTENSION) throw new Error('Set REST_EXT to the built rest_ext.duckdb_extension');

/** Every request the server saw: method, url, headers, body. */
const seen = [];
let routes = {};
let server;
let base;

before(async () => {
  server = createServer((req, res) => {
    let body = '';
    req.on('data', (chunk) => (body += chunk));
    req.on('end', () => {
      seen.push({ method: req.method, url: req.url, headers: req.headers, body });
      const path = req.url.split('?')[0];
      const route = routes[path] ?? routes[req.url];
      if (!route) {
        res.writeHead(404).end('{"error":"no route"}');
        return;
      }
      route(req, res, body);
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  base = `http://127.0.0.1:${server.address().port}`;
});

after(() => server.close());

const json = (res, value, status = 200) => res.writeHead(status, { 'Content-Type': 'application/json' }).end(JSON.stringify(value));

async function open() {
  const db = await DuckDBInstance.create(':memory:', { allow_unsigned_extensions: 'true', autoload_known_extensions: 'false' });
  const connection = await db.connect();
  await connection.run(`LOAD '${EXTENSION}'`);
  const rows = async (sql) => (await connection.runAndReadAll(sql)).getRowObjectsJson();
  return { connection, rows, close: () => { connection.closeSync(); db.closeSync(); } };
}

/** Runs `fn` with fresh routes and a cleared request log. */
async function withRoutes(table, fn) {
  routes = table;
  seen.length = 0;
  const duck = await open();
  try {
    await fn(duck);
  } finally {
    duck.close();
  }
}

describe('without pagination options', () => {
  it('still returns an array response as rows and non-JSON as one text row', async () => {
    await withRoutes({
      '/users': (_q, res) => json(res, [{ id: 1, name: 'a' }, { id: 2, name: 'b' }]),
      '/page': (_q, res) => res.writeHead(200, { 'Content-Type': 'text/html' }).end('<html>hi</html>'),
    }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/users method=GET' AS users (TYPE rest_ext)`);
      assert.deepEqual(await rows(`SELECT id, name FROM users('{}', '{}') ORDER BY id`), [{ id: '1', name: 'a' }, { id: '2', name: 'b' }]);
      await rows(`ATTACH 'url=${base}/page method=GET' AS page (TYPE rest_ext)`);
      assert.deepEqual(await rows(`SELECT result FROM page('{}', '{}')`), [{ result: '<html>hi</html>' }]);
    });
  });
});

describe('ITEMS and NEXT_URL', () => {
  const pages = (count) => (req, res, body) => {
    const n = Number(new URL(req.url, base).searchParams.get('page') ?? '1');
    json(res, {
      properties: {
        rows: [[n * 10 + 1, `p${n}`], [n * 10 + 2, `p${n}`]],
        nextLink: n < count ? `${base}/costs?page=${n + 1}` : null,
      },
      echoedBody: body,
    });
  };

  it('walks every page, re-sending the POST body, and yields each page\'s rows', async () => {
    await withRoutes({ '/costs': pages(3) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/costs method=POST' AS costs (TYPE rest_ext, ITEMS '/properties/rows', NEXT_URL '/properties/nextLink')`);
      const result = await rows(`SELECT value[1] AS amount, value[2] AS page FROM costs('{}', '{"type":"ActualCost"}')`);
      assert.deepEqual(result.map((row) => row.amount), ['11', '12', '21', '22', '31', '32']);
      assert.equal(seen.length, 3);
      assert.ok(seen.every((request) => request.method === 'POST' && request.body === '{"type":"ActualCost"}'));
    });
  });

  it('fetches only the pages a LIMIT needs', async () => {
    await withRoutes({ '/costs': pages(50) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/costs method=POST' AS costs (TYPE rest_ext, ITEMS '/properties/rows', NEXT_URL '/properties/nextLink')`);
      assert.equal((await rows(`SELECT * FROM costs('{}', '{}') LIMIT 3`)).length, 3);
      assert.ok(seen.length <= 3, `made ${seen.length} requests for 3 rows`);
    });
  });

  it('refuses a next link on another origin, before requesting it', async () => {
    await withRoutes({
      '/costs': (_q, res) => json(res, { properties: { rows: [[1, 'a']], nextLink: base.replace('127.0.0.1', 'localhost') + '/costs?page=2' } }),
    }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/costs method=POST' AS costs (TYPE rest_ext, ITEMS '/properties/rows', NEXT_URL '/properties/nextLink')`);
      await assert.rejects(rows(`SELECT * FROM costs('{}', '{}')`), /not on the endpoint's origin/);
      assert.equal(seen.length, 1);
    });
  });

  it('stops at MAX_PAGES, and on a cursor that repeats', async () => {
    await withRoutes({ '/costs': pages(100) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/costs method=POST' AS costs (TYPE rest_ext, ITEMS '/properties/rows', NEXT_URL '/properties/nextLink', MAX_PAGES 4)`);
      await assert.rejects(rows(`SELECT count(*) FROM costs('{}', '{}')`), /MAX_PAGES \(4\)/);
      assert.equal(seen.length, 4);
    });
    await withRoutes({
      '/loop': (_q, res) => json(res, { data: [{ id: 1 }], next: `${base}/loop?again=1` }),
    }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/loop method=GET' AS loop (TYPE rest_ext, ITEMS '/data', NEXT_URL '/next')`);
      await assert.rejects(rows(`SELECT count(*) FROM loop('{}', '{}')`), /same next-page cursor twice/);
    });
  });
});

describe('NEXT_TOKEN', () => {
  it('writes the cursor into the request body at TOKEN_BODY', async () => {
    await withRoutes({
      '/graph': (_q, res, body) => {
        const token = JSON.parse(body).options?.$skipToken;
        const page = token ? Number(token.slice(1)) : 1;
        json(res, { data: [{ id: `r${page}` }], $skipToken: page < 3 ? `t${page + 1}` : undefined });
      },
    }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/graph method=POST' AS graph (TYPE rest_ext, ITEMS '/data', NEXT_TOKEN '/$skipToken', TOKEN_BODY '/options/$skipToken')`);
      const result = await rows(`SELECT id FROM graph('{}', '{"query":"Resources","options":{"resultFormat":"objectArray"}}')`);
      assert.deepEqual(result.map((row) => row.id), ['r1', 'r2', 'r3']);
      assert.deepEqual(JSON.parse(seen[1].body), { query: 'Resources', options: { resultFormat: 'objectArray', $skipToken: 't2' } });
    });
  });

  it('sends the cursor as TOKEN_PARAM alongside the caller\'s own parameters', async () => {
    await withRoutes({
      '/items': (req, res) => {
        const cursor = new URL(req.url, base).searchParams.get('cursor');
        json(res, cursor ? { items: [{ n: 2 }], next: null } : { items: [{ n: 1 }], next: 'abc' });
      },
    }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/items method=GET' AS items (TYPE rest_ext, ITEMS '/items', NEXT_TOKEN '/next', TOKEN_PARAM 'cursor')`);
      assert.deepEqual((await rows(`SELECT n FROM items('{"q":"x"}', '{}')`)).map((row) => row.n), ['1', '2']);
      const second = new URL(seen[1].url, base).searchParams;
      assert.deepEqual([second.get('q'), second.get('cursor')], ['x', 'abc']);
    });
  });

  it('infers the schema from the first page with rows, and refuses a later row that does not fit it', async () => {
    await withRoutes({
      '/sparse': (req, res) => {
        const cursor = new URL(req.url, base).searchParams.get('c');
        if (!cursor) return json(res, { items: [], next: '2' });
        if (cursor === '2') return json(res, { items: [{ n: 1 }], next: '3' });
        return json(res, { items: [{ n: 'not a number' }], next: null });
      },
    }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/sparse method=GET' AS sparse (TYPE rest_ext, ITEMS '/items', NEXT_TOKEN '/next', TOKEN_PARAM 'c')`);
      await assert.rejects(rows(`SELECT n FROM sparse('{}', '{}')`), /"not a number" does not fit the column type BIGINT/);
      assert.deepEqual(await rows(`SELECT n FROM sparse('{}', '{}') LIMIT 1`), [{ n: '1' }]);
    });
  });

  it('refuses option combinations that cannot work', async () => {
    await withRoutes({}, async ({ rows }) => {
      await assert.rejects(rows(`ATTACH 'url=${base}/x' AS a (TYPE rest_ext, NEXT_TOKEN '/t')`), /exactly one of TOKEN_BODY or TOKEN_PARAM/);
      await assert.rejects(rows(`ATTACH 'url=${base}/x' AS b (TYPE rest_ext, NEXT_URL '/n', NEXT_TOKEN '/t', TOKEN_PARAM 'c')`), /one of NEXT_URL, NEXT_TOKEN, PAGE_PARAM and OFFSET_PARAM, not several/);
      await assert.rejects(rows(`ATTACH 'url=${base}/x' AS c (TYPE rest_ext, ITEMS 'data')`), /JSON pointer starting with '\/'/);
      await assert.rejects(rows(`ATTACH 'url=${base}/x' AS d (TYPE rest_ext, NEXT_URL '/n', MAX_PAGES 0)`), /MAX_PAGES must be an integer of at least 1/);
    });
  });
});

describe('PAGE_PARAM and OFFSET_PARAM', () => {
  // 7 rows served three at a time: by page number (from `first`) or by offset.
  const ROWS = [1, 2, 3, 4, 5, 6, 7].map((n) => ({ n }));
  const byPage = (param, first) => (req, res) => {
    const page = Number(new URL(req.url, base).searchParams.get(param));
    json(res, { data: ROWS.slice((page - first) * 3, (page - first) * 3 + 3) });
  };
  const byOffset = (param) => (req, res) => {
    const offset = Number(new URL(req.url, base).searchParams.get(param));
    json(res, { host_list: ROWS.slice(offset, offset + 3), total: ROWS.length });
  };
  const sent = (param) => seen.map((request) => new URL(request.url, base).searchParams.get(param));

  it('counts page numbers up from PAGE_START, beside the caller\'s own parameters, until a page is empty', async () => {
    await withRoutes({ '/users': byPage('page[number]', 0) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/users method=GET' AS users (TYPE rest_ext, ITEMS '/data', PAGE_PARAM 'page[number]')`);
      assert.deepEqual((await rows(`SELECT n FROM users('{"page[size]":"3"}', '{}')`)).map((row) => row.n), ['1', '2', '3', '4', '5', '6', '7']);
      assert.deepEqual(sent('page[number]'), ['0', '1', '2', '3']);
      assert.ok(seen.every((request) => new URL(request.url, base).searchParams.get('page[size]') === '3'));
    });
    await withRoutes({ '/users': byPage('page', 1) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/users method=GET' AS users (TYPE rest_ext, ITEMS '/data', PAGE_PARAM 'page', PAGE_START 1)`);
      assert.equal((await rows(`SELECT count(*) AS c FROM users('{}', '{}')`))[0].c, '7');
      assert.deepEqual(sent('page'), ['1', '2', '3', '4']);
    });
  });

  it('sends the rows read so far as OFFSET_PARAM, and with PAGE_SIZE stops at a short page without asking again', async () => {
    await withRoutes({ '/hosts': byOffset('start') }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/hosts method=GET' AS hosts (TYPE rest_ext, ITEMS '/host_list', OFFSET_PARAM 'start', PAGE_SIZE 3)`);
      assert.deepEqual((await rows(`SELECT n FROM hosts('{"count":"3"}', '{}')`)).map((row) => row.n), ['1', '2', '3', '4', '5', '6', '7']);
      assert.deepEqual(sent('start'), ['0', '3', '6']);
    });
  });

  it('fetches only the pages a LIMIT needs, and stops at MAX_PAGES', async () => {
    await withRoutes({ '/hosts': byOffset('start') }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/hosts method=GET' AS hosts (TYPE rest_ext, ITEMS '/host_list', OFFSET_PARAM 'start')`);
      assert.equal((await rows(`SELECT * FROM hosts('{}', '{}') LIMIT 2`)).length, 2);
      assert.equal(seen.length, 1);
    });
    await withRoutes({ '/users': (_q, res) => json(res, { data: [{ n: 1 }] }) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/users method=GET' AS users (TYPE rest_ext, ITEMS '/data', PAGE_PARAM 'p', MAX_PAGES 5)`);
      await assert.rejects(rows(`SELECT count(*) FROM users('{}', '{}')`), /MAX_PAGES \(5\)/);
      assert.equal(seen.length, 5);
    });
  });

  it('refuses option combinations that cannot work', async () => {
    await withRoutes({}, async ({ rows }) => {
      await assert.rejects(rows(`ATTACH 'url=${base}/x' AS a (TYPE rest_ext, PAGE_PARAM 'p', NEXT_URL '/n')`), /not several/);
      await assert.rejects(rows(`ATTACH 'url=${base}/x' AS b (TYPE rest_ext, PAGE_PARAM 'p', OFFSET_PARAM 'o')`), /not several/);
      await assert.rejects(rows(`ATTACH 'url=${base}/x' AS c (TYPE rest_ext, OFFSET_PARAM 'o', PAGE_START 1)`), /PAGE_START only applies with PAGE_PARAM/);
      await assert.rejects(rows(`ATTACH 'url=${base}/x' AS d (TYPE rest_ext, NEXT_URL '/n', PAGE_SIZE 10)`), /PAGE_SIZE only applies with PAGE_PARAM or OFFSET_PARAM/);
      await assert.rejects(rows(`ATTACH 'url=${base}/x' AS e (TYPE rest_ext, PAGE_PARAM 'p', PAGE_START -1)`), /PAGE_START must be an integer of at least 0/);
    });
  });
});

describe('COLUMNS and COLUMN_TYPES', () => {
  const tabular = (req, res) => {
    const page = Number(new URL(req.url, base).searchParams.get('page') ?? '1');
    json(res, {
      properties: {
        columns: [{ name: 'Cost', type: 'Number' }, { name: 'UsageDate', type: 'Number' }, { name: 'ServiceName', type: 'String' }],
        rows: page === 1 ? [[0, 20260901, 'Storage'], [3, 20260901, 'Compute']] : [[1.25, 20260902, 'Storage']],
        nextLink: page === 1 ? `${base}/tabular?page=2` : null,
      },
    });
  };

  it('names positional rows after the column list, and keeps declared types across pages', async () => {
    await withRoutes({ '/tabular': tabular }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/tabular method=POST' AS t (TYPE rest_ext, ITEMS '/properties/rows', COLUMNS '/properties/columns', NEXT_URL '/properties/nextLink', COLUMN_TYPES '{"Cost":"DOUBLE","UsageDate":"BIGINT","ServiceName":"VARCHAR"}')`);
      const result = await rows(`SELECT Cost, UsageDate, ServiceName, typeof(Cost) AS cost_type FROM t('{}', '{}')`);
      assert.deepEqual(result.map((row) => [row.Cost, row.UsageDate, row.ServiceName]), [
        [0, '20260901', 'Storage'], [3, '20260901', 'Compute'], [1.25, '20260902', 'Storage'],
      ]);
      assert.equal(result[0].cost_type, 'DOUBLE');
    });
  });

  it('without a declared type, a first page of whole numbers makes a later fraction an error', async () => {
    await withRoutes({ '/tabular': tabular }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/tabular method=POST' AS t (TYPE rest_ext, ITEMS '/properties/rows', COLUMNS '/properties/columns', NEXT_URL '/properties/nextLink')`);
      await assert.rejects(rows(`SELECT * FROM t('{}', '{}')`), /1\.25 does not fit the column type BIGINT/);
    });
  });

  it('declares the whole schema, so an empty result has it and undeclared fields are left out', async () => {
    await withRoutes({
      '/none': (_q, res) => json(res, { data: [] }),
      '/some': (_q, res) => json(res, { data: [{ id: 'a', count: 2, extra: true }] }),
    }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/none method=POST' AS empty_api (TYPE rest_ext, ITEMS '/data', COLUMN_TYPES '{"id":"VARCHAR","count":"BIGINT"}')`);
      assert.deepEqual(await rows(`SELECT id, count FROM empty_api('{}', '{}')`), []);
      await rows(`ATTACH 'url=${base}/some method=POST' AS some_api (TYPE rest_ext, ITEMS '/data', COLUMN_TYPES '{"id":"VARCHAR","count":"BIGINT"}')`);
      assert.deepEqual(await rows(`SELECT * FROM some_api('{}', '{}')`), [{ id: 'a', count: '2' }]);
      for (const type of ['DATE', 'MAP(VARCHAR, VARCHAR)', 'STRUCT(ts TIMESTAMP)[]']) {
        await assert.rejects(rows(`ATTACH 'url=${base}/none' AS bad (TYPE rest_ext, COLUMN_TYPES '{"id":"${type}"}')`), /must be BOOLEAN, BIGINT, DOUBLE, VARCHAR, or a LIST or STRUCT of those/, type);
      }
    });
  });

  it('with a declared schema, makes no request until the scan: a view, PREPARE or EXPLAIN costs nothing', async () => {
    await withRoutes({ '/lazy': (_q, res) => json(res, { data: [{ id: 'a' }], next: null }) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/lazy method=POST' AS lazy (TYPE rest_ext, ITEMS '/data', NEXT_URL '/next', COLUMN_TYPES '{"id":"VARCHAR"}')`);
      await rows(`CREATE VIEW lazy_view AS SELECT id FROM lazy('{}', '{}')`);
      await rows(`PREPARE lazy_query AS SELECT id FROM lazy_view`);
      await rows(`EXPLAIN SELECT id FROM lazy_view`);
      assert.equal(seen.length, 0);
      assert.deepEqual(await rows(`SELECT id FROM lazy_view`), [{ id: 'a' }]);
      assert.equal(seen.length, 1);
    });
  });

  it('reads nested declared types', async () => {
    await withRoutes({
      '/metrics': (_q, res) => json(res, { value: [{ name: { value: 'CPU' }, timeseries: [{ data: [{ timeStamp: '2026-09-01T00:00:00Z', average: 4 }, { timeStamp: '2026-09-02T00:00:00Z' }] }] }] }),
    }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/metrics method=GET' AS m (TYPE rest_ext, ITEMS '/value', COLUMN_TYPES '{"name":"STRUCT(value VARCHAR)","timeseries":"STRUCT(data STRUCT(\\"timeStamp\\" VARCHAR, average DOUBLE)[])[]"}')`);
      const points = await rows(`SELECT name.value AS metric, p.point.timeStamp AS stamp, p.point.average AS average
        FROM m('{}', '{}'), unnest(timeseries) AS s(series), unnest(s.series.data) AS p(point) ORDER BY stamp`);
      assert.deepEqual(points, [
        { metric: 'CPU', stamp: '2026-09-01T00:00:00Z', average: 4 },
        { metric: 'CPU', stamp: '2026-09-02T00:00:00Z', average: null },
      ]);
    });
  });
});

describe('query parameters and JSON errors', () => {
  it('sends numeric query parameters exactly', async () => {
    await withRoutes({ '/n': (_q, res) => json(res, { ok: true }) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/n method=GET' AS n (TYPE rest_ext)`);
      await rows(`SELECT * FROM n('{"date":20260901,"big":9007199254740993,"ratio":0.1}', '{}')`);
      const params = new URL(seen[0].url, base).searchParams;
      assert.deepEqual([params.get('date'), params.get('big'), params.get('ratio')], ['20260901', '9007199254740993', '0.10000000000000001']);
    });
  });

  it('does not repeat malformed headers JSON, which holds credentials, in the error', async () => {
    await withRoutes({}, async ({ rows }) => {
      await assert.rejects(rows(`ATTACH 'url=${base}/x headers={"Authorization":"Bearer sekrit" method=GET' AS h (TYPE rest_ext)`), (error) => {
        assert.match(error.message, /failed to parse a JSON object/);
        assert.doesNotMatch(error.message, /sekrit/);
        return true;
      });
    });
  });
});

describe('interrupting a paginated scan', () => {
  it('stops between pages', async () => {
    await withRoutes({
      '/slow': (req, res) => {
        const page = Number(new URL(req.url, base).searchParams.get('p') ?? '1');
        setTimeout(() => json(res, { data: [{ page }], next: `${base}/slow?p=${page + 1}` }), 200);
      },
    }, async ({ connection, rows }) => {
      await rows(`ATTACH 'url=${base}/slow method=GET' AS slow (TYPE rest_ext, ITEMS '/data', NEXT_URL '/next')`);
      const timer = setTimeout(() => connection.interrupt(), 700);
      await assert.rejects(rows(`SELECT count(*) FROM slow('{}', '{}')`), /INTERRUPT|Interrupted/i);
      clearTimeout(timer);
      const requests = seen.length;
      await new Promise((resolve) => setTimeout(resolve, 600));
      assert.ok(seen.length <= requests + 1, 'kept fetching pages after the interrupt');
    });
  });
});

describe('request safety', () => {
  it('refuses a header with a CR or LF in it, before sending anything', async () => {
    await withRoutes({ '/h': (_q, res) => json(res, { ok: true }) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/h headers={"X-A":"1\\r\\nX-Injected: yes"} method=GET' AS h (TYPE rest_ext)`);
      await assert.rejects(rows(`SELECT * FROM h('{}', '{}')`), /contains a control character/);
      assert.equal(seen.length, 0);
    });
  });

  it('percent-encodes a path placeholder, and refuses ".." or a host-changing value', async () => {
    await withRoutes({ '/items/a%3Fb%23c/x': (_q, res) => json(res, { ok: true }) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/items/{id}/x method=GET' AS item (TYPE rest_ext)`);
      assert.deepEqual(await rows(`SELECT ok FROM item('{"id":"a?b#c"}', '{}')`), [{ ok: true }]);
      assert.equal(seen[0].url, '/items/a%3Fb%23c/x');
      await assert.rejects(rows(`SELECT * FROM item('{"id":"../admin"}', '{}')`), /may not contain a "\.\." path segment/);
      await rows(`ATTACH 'url=http://{host}:1/x method=GET' AS hosted (TYPE rest_ext)`);
      await assert.rejects(rows(`SELECT * FROM hosted('{"host":"evil.example/"}', '{}')`), /part of the host name/);
    });
  });

  it('follows a redirect on the same origin, and refuses one to another origin', async () => {
    await withRoutes({
      '/old': (_q, res) => res.writeHead(302, { Location: '/new' }).end(),
      '/new': (_q, res) => json(res, { moved: true }),
      '/away': (_q, res) => res.writeHead(302, { Location: base.replace('127.0.0.1', 'localhost') + '/new' }).end(),
    }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/old headers={"X-Api-Key":"k"} method=GET' AS old (TYPE rest_ext)`);
      assert.deepEqual(await rows(`SELECT moved FROM old('{}', '{}')`), [{ moved: true }]);
      assert.equal(seen[1].headers['x-api-key'], 'k');
      seen.length = 0;
      await rows(`ATTACH 'url=${base}/away headers={"X-Api-Key":"k"} method=GET' AS away (TYPE rest_ext)`);
      await assert.rejects(rows(`SELECT * FROM away('{}', '{}')`), /redirected to another origin/);
      assert.equal(seen.length, 1);
    });
  });

  it('applies a header secret only where its SCOPE ends on a URL boundary', async () => {
    await withRoutes({ '/scoped/api': (_q, res) => json(res, { ok: true }) }, async ({ rows }) => {
      const port = String(server.address().port);
      const partialPort = `http://127.0.0.1:${port.slice(0, -1)}`;
      await rows(`CREATE SECRET partial_port (TYPE rest_ext_headers, SCOPE '${partialPort}', HEADERS MAP {'X-Secret': 'partial-port'})`);
      await rows(`CREATE SECRET partial_path (TYPE rest_ext_headers, SCOPE '${base}/scoped/ap', HEADERS MAP {'X-Secret': 'partial-path'})`);
      await rows(`ATTACH 'url=${base}/scoped/api method=GET' AS unmatched (TYPE rest_ext)`);
      await rows(`SELECT * FROM unmatched('{}', '{}')`);
      assert.equal(seen[0].headers['x-secret'], undefined);

      await rows(`CREATE SECRET origin (TYPE rest_ext_headers, SCOPE '${base}', HEADERS MAP {'X-Secret': 'origin'})`);
      await rows(`CREATE SECRET path (TYPE rest_ext_headers, SCOPE '${base}/scoped', HEADERS MAP {'X-Secret': 'path'})`);
      await rows(`ATTACH 'url=${base}/scoped/api method=GET' AS matched (TYPE rest_ext)`);
      await rows(`SELECT * FROM matched('{}', '{}')`);
      assert.equal(seen[1].headers['x-secret'], 'path');
    });
  });

  it('refuses ATTACH once enable_external_access is off, while endpoints attached before keep working', async () => {
    await withRoutes({ '/ok': (_q, res) => json(res, { ok: true }) }, async ({ rows }) => {
      await rows(`ATTACH 'url=${base}/ok method=GET' AS before_lock (TYPE rest_ext)`);
      await rows('SET enable_external_access = false');
      await assert.rejects(rows(`ATTACH 'url=${base}/ok method=GET' AS after_lock (TYPE rest_ext)`), /enable_external_access is false/);
      assert.deepEqual(await rows(`SELECT ok FROM before_lock('{}', '{}')`), [{ ok: true }]);
    });
  });
});
