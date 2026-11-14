#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

pattern='cpu_accel_(flush|drain|destroy)_workqueue'

printf 'file\tflush\tdrain\tdestroy\tverdict\n'

git grep -l -E "$pattern" -- \
	':!Documentation' \
	':!tools' \
	':!drivers/cpu_accel' \
	':!kernel/workqueue.c' \
	':!include/linux/workqueue.h' \
	':!include/linux/cpu_accel_workqueue.h' \
	':!MAINTAINERS' |
sort |
while IFS= read -r file; do
	flush=$(grep -c 'cpu_accel_flush_workqueue' "$file" || true)
	drain=$(grep -c 'cpu_accel_drain_workqueue' "$file" || true)
	destroy=$(grep -c 'cpu_accel_destroy_workqueue' "$file" || true)
	printf '%s\t%s\t%s\t%s\tpending\n' \
		"$file" "$flush" "$drain" "$destroy"
done
