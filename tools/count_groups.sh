#!/bin/sh
# Counts behavioural test groups across every suite in a build directory.
#
# Each suite prints one "  PASS  <group>" or "  FAIL  <group>" line per group, so
# the total number of groups is the number of those lines.
BUILD="${1:-build}"
total=0
for binary in "$BUILD"/*_test; do
  [ -x "$binary" ] || continue
  name=$(basename "$binary")
  count=$("$binary" 2>/dev/null | grep -c '^  \(PASS\|FAIL\)  ' || true)
  printf '%-28s %s\n' "$name" "$count"
  total=$((total + count))
done
printf '%-28s %s\n' "TOTAL" "$total"
