# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the quack extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

`test/sql/rest_ext_calls.test` (retries, bounds, `Link` paging, `rest_ext_calls()`) talks to the local
HTTP server in `test/fixtures/fixture_server.py` and is skipped unless `REST_EXT_FIXTURE_URL` is set.
`test/run_fixture_tests.sh` starts the server, sets that variable and runs the test with the built
`unittest` binary (`UNITTEST=...` to use another).
