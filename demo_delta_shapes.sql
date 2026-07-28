-- Run from the duckdb-delta repository root with:
--   ./build/debug/duckdb -unsigned -init demo_delta_shapes.sql
-- Or from an open shell:
--   .read demo_delta_shapes.sql

.mode duckbox
.timer on

.print '\n=== 1. Simple JSON-log table ==='
SELECT count(*) AS rows, min(i) AS min_i, max(i) AS max_i
FROM delta_scan_data_ir('data/inlined/simple_table/delta_lake');

.print '\n=== 2. Partitioned checkpoint table ==='
SELECT count(*) AS rows, min(part) AS min_partition, max(part) AS max_partition
FROM delta_scan_data_ir('data/inlined/issue_303_partitioned/delta_lake');

.print '\n=== 3. Time travel over the partitioned table ==='
SELECT 5 AS version, count(*) AS rows
FROM delta_scan_data_ir('data/inlined/issue_303_partitioned/delta_lake', version = 5);

.print '\n=== 4. Deletion-vector table (surviving values are 1..8) ==='
SELECT *
FROM delta_scan_data_ir('vendor/delta-kernel-rs/kernel/tests/data/table-with-dv-small')
ORDER BY value;

.print '\n=== 5. Ordinary partitioned table ==='
SELECT *
FROM delta_scan_data_ir('vendor/delta-kernel-rs/kernel/tests/data/basic_partitioned')
ORDER BY number;

.print '\n=== 6. Decimal table ==='
SELECT *
FROM delta_scan_data_ir('vendor/delta-kernel-rs/kernel/tests/data/basic-decimal-table')
ORDER BY part, col1;

.print '\n=== 7. TIMESTAMP_NTZ and timestamp partition ==='
SELECT *
FROM delta_scan_data_ir('vendor/delta-kernel-rs/kernel/tests/data/data-reader-timestamp_ntz')
ORDER BY id;

.print '\n=== 8. Nested all-types/row-group fixture ==='
SELECT count(*) AS rows, count(missing) AS non_null_missing
FROM delta_scan_data_ir('vendor/delta-kernel-rs/kernel/tests/data/parquet_row_group_skipping');

.print '\n=== 9. Metadata-only checkpoint reconciliation ==='
EXPLAIN ANALYZE
SELECT *
FROM delta_scan_metadata('data/inlined/issue_303_partitioned/delta_lake');

.print '\n=== 10. File-statistics skipping: baseline (six data files) ==='
EXPLAIN ANALYZE
SELECT count(*)
FROM delta_scan('vendor/delta-kernel-rs/kernel/tests/data/parsed-stats');

.print '\n=== 11. File-statistics skipping: selective range (one data file) ==='
EXPLAIN ANALYZE
SELECT count(*)
FROM delta_scan('vendor/delta-kernel-rs/kernel/tests/data/parsed-stats')
WHERE id BETWEEN 250 AND 260;

.print '\n=== 12. Same selective predicate through the full terminal IR (11 rows) ==='
-- This demonstrates full-data-plan correctness. The production delta_scan profile above is
-- the command that currently demonstrates optimizer-to-kernel file-statistics pushdown.
SELECT count(*) AS rows
FROM delta_scan_data_ir('vendor/delta-kernel-rs/kernel/tests/data/parsed-stats')
WHERE id BETWEEN 250 AND 260;
