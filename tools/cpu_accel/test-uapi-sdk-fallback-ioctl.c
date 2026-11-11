// SPDX-License-Identifier: GPL-2.0-only

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#include <linux/cpu_accel.h>

int ioctl(int fd, unsigned long command, ...)
{
	static int (*real_ioctl)(int, unsigned long, ...);
	va_list arguments;
	void *argument;

	if (command == CPU_ACCEL_IOC_GET_ABI_INFO) {
		errno = ENOTTY;
		return -1;
	}
	if (!real_ioctl) {
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");
		if (!real_ioctl) {
			errno = ELIBBAD;
			return -1;
		}
	}
	va_start(arguments, command);
	argument = va_arg(arguments, void *);
	va_end(arguments);
	return real_ioctl(fd, command, argument);
}
