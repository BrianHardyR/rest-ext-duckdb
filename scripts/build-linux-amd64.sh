#!/usr/bin/env bash
# Builds dist-repo/<version>/linux_amd64/rest_ext.duckdb_extension.gz inside Debian bookworm, so the
# binary needs glibc 2.34 at most and runs on bookworm (2.36), Ubuntu 22.04 and newer. A binary built
# on the host needs that host's glibc (a 2.38 build refuses to load on bookworm).
#
# Usage: scripts/build-linux-amd64.sh v1.5.5
#   with ./duckdb checked out at that tag and ./extension-ci-tools populated (git submodule update).
#   JOBS=4 raises build parallelism; each compile job wants ~1.5 GB of memory.
set -euo pipefail

VERSION="${1:?usage: $0 <duckdb version tag, e.g. v1.5.5>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${JOBS:-2}"
BUILD="build/linux_amd64_${VERSION}"

docker build --platform linux/amd64 -q -t rest-ext-builder:bookworm - <<'DOCKERFILE'
FROM debian:bookworm
RUN apt-get update -qq \
 && apt-get install -y -qq build-essential cmake ninja-build libssl-dev python3 >/dev/null \
 && rm -rf /var/lib/apt/lists/*
DOCKERFILE

docker run --rm --platform linux/amd64 -v "$ROOT:/src" -w /src rest-ext-builder:bookworm bash -c "
  set -e
  cmake -G Ninja -S duckdb -B '$BUILD' -DCMAKE_BUILD_TYPE=Release \
    -DDUCKDB_EXTENSION_CONFIGS=/src/extension_config.cmake -DEXTENSION_STATIC_BUILD=1 \
    -DDUCKDB_EXPLICIT_PLATFORM=linux_amd64 -DOVERRIDE_GIT_DESCRIBE='$VERSION' \
    -DOPENSSL_USE_STATIC_LIBS=TRUE -DBUILD_SHELL=0 -DBUILD_UNITTESTS=0 >/dev/null
  ninja -C '$BUILD' -j '$JOBS' rest_ext_loadable_extension
  glibc=\$(objdump -T '$BUILD/extension/rest_ext/rest_ext.duckdb_extension' | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
  echo \"needs \$glibc\"
  mkdir -p 'dist-repo/$VERSION/linux_amd64'
  gzip -9 -n -c '$BUILD/extension/rest_ext/rest_ext.duckdb_extension' > 'dist-repo/$VERSION/linux_amd64/rest_ext.duckdb_extension.gz'
"
echo "wrote dist-repo/$VERSION/linux_amd64/rest_ext.duckdb_extension.gz"
