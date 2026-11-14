#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

wrapper_pattern='cpu_accel_(flush|drain|destroy)_workqueue'

git diff --name-only 40288c9206c1..HEAD |
rg -v '^(Documentation/|tools/|drivers/cpu_accel/|MAINTAINERS$)' |
while IFS= read -r file; do
	if git diff -U0 40288c9206c1..HEAD -- "$file" |
		rg -q "$wrapper_pattern"; then
		continue
	fi
	case "$file" in
	arch/x86/include/asm/cpu_accel.h|\
	arch/x86/kernel/accel.c|\
	include/linux/irq_accel.h|\
	include/linux/mmu_owner.h|\
	include/linux/rcu_nocb.h|\
	include/trace/events/cpu_accel.h|\
	include/uapi/linux/cpu_accel.h|\
	kernel/irq/accel_test.c|\
	mm/owner.c)
		continue
		;;
	esac
	printf '%s\n' "$file"
done |
sort |
while IFS= read -r file; do
	case "$file" in
	arch/x86/*) area='arch/x86' ;;
	drivers/*) area='drivers' ;;
	fs/*) area='fs' ;;
	include/*) area='include' ;;
	kernel/*) area='kernel' ;;
	mm/*) area='mm' ;;
	net/*) area='net' ;;
	samples/*) area='samples' ;;
	*) area='other' ;;
	esac
	printf '%s\t%s\tpending\n' "$file" "$area"
done
