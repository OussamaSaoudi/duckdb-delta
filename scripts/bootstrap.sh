#!/bin/sh
set -eu

build_type=${1:-debug}
case "$build_type" in
  debug|release) ;;
  *)
    echo "usage: $0 [debug|release]" >&2
    exit 2
    ;;
esac

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_dir"

git submodule update --init --recursive
make "$build_type"

duckdb_cli="$repo_dir/build/$build_type/duckdb"
echo "Built: $duckdb_cli"
echo "Run:   $duckdb_cli"
echo "Demo:  cd $repo_dir && ./build/$build_type/duckdb -unsigned -init demo_delta_shapes.sql"
