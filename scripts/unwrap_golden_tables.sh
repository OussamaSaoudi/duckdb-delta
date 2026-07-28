#!/bin/bash

rm -rf data/unpacked_golden_tables
mkdir -p data/unpacked_golden_tables
cp vendor/delta-kernel-rs/kernel/tests/golden_data/*.zst data/unpacked_golden_tables
for f in data/unpacked_golden_tables/*.zst; do tar -xvf "$f" -C data/unpacked_golden_tables; rm $f; done
