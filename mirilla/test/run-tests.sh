#!/bin/sh
#
# Run every mirilla test suite in order and aggregate the outcomes.
#
# Requires root and the mirilla module loaded (`/dev/mirilla` present).

set -u

SUITE_LIST="test-suite test-self test-concurrency test-invariants test-ioctl"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ ! -c /dev/mirilla ]; then
	echo "error: /dev/mirilla is absent; load the mirilla module first" >&2
	exit 1
fi

failed_suites=""

for suite in $SUITE_LIST; do
	echo ""
	echo "==> Running $suite"

	if ! "$SCRIPT_DIR/$suite"; then
		failed_suites="$failed_suites $suite"
	fi
done

echo ""
echo "========================================"
echo "         SUITE SUMMARY"
echo "========================================"

if [ -n "$failed_suites" ]; then
	echo "FAILED:$failed_suites"
	exit 1
fi

echo "All suites passed."
exit 0
