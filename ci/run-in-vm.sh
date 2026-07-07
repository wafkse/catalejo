#!/bin/sh
#
# Guest-side test driver: load the module, run every suite, unload, and
# hand the aggregate status back to the host through the vng exit code.
#
# virtme-ng shares the host filesystem into the guest, so the repository
# is reachable at its host path.

set -u

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(dirname "$SCRIPT_DIR")

echo "Running the mirilla suites under $(uname -r)"

if ! insmod "$REPO_ROOT/mirilla/mirilla.ko"; then
	echo "error: failed to load mirilla.ko" >&2
	dmesg | tail -50
	exit 1
fi

# devtmpfs materializes the node; fall back to mknod when it did not.
if [ ! -c /dev/mirilla ]; then
	major=$(awk '$2 == "mirilla" { print $1 }' /proc/devices)

	if [ -z "$major" ]; then
		echo "error: mirilla character device is absent" >&2
		exit 1
	fi

	mknod /dev/mirilla c "$major" 0
fi

"$REPO_ROOT/mirilla/test/run-tests.sh"
status=$?

# Surface the module's kernel-side view of any failure.
[ "$status" -eq 0 ] || dmesg | tail -100

rmmod mirilla

exit $status
