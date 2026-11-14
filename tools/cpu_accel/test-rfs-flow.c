// SPDX-License-Identifier: GPL-2.0-only

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void fail(const char *message)
{
	perror(message);
	exit(1);
}

static int parse_endpoint(const char *address, const char *port,
			  struct sockaddr_in *endpoint, int allow_zero)
{
	char *end;
	unsigned long port_number;

	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->sin_family = AF_INET;
	if (inet_pton(AF_INET, address, &endpoint->sin_addr) != 1) {
		fprintf(stderr, "invalid IPv4 address: %s\n", address);
		return -1;
	}

	errno = 0;
	port_number = strtoul(port, &end, 10);
	if (errno || *end || port_number > 65535 ||
	    (!allow_zero && port_number == 0)) {
		fprintf(stderr, "invalid UDP port: %s\n", port);
		return -1;
	}
	endpoint->sin_port = htons((uint16_t)port_number);
	return 0;
}

static void run_receiver(const char *address, const char *port)
{
	struct sockaddr_in endpoint;
	char buffer[4096];
	int fd;

	if (parse_endpoint(address, port, &endpoint, 0))
		exit(1);
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		fail("socket");
	if (bind(fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) < 0)
		fail("bind");

	for (;;) {
		ssize_t length = recv(fd, buffer, sizeof(buffer), 0);

		if (length < 0)
			fail("recv");
		if (write(STDOUT_FILENO, buffer, length) != length)
			fail("write payload");
		if (write(STDOUT_FILENO, "\n", 1) != 1)
			fail("write newline");
	}
}

static void run_sender(const char *address, const char *port,
		       const char *source, const char *device)
{
	struct sockaddr_in destination;
	struct sockaddr_in local;
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length;
	int fd;

	if (parse_endpoint(address, port, &destination, 0))
		exit(1);
	if (parse_endpoint(source, "0", &local, 1))
		exit(1);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		fail("socket");
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, device,
		       strlen(device)) < 0)
		fail("SO_BINDTODEVICE");
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0)
		fail("bind");
	if (printf("ready\n") < 0 || fflush(stdout) == EOF)
		fail("stdout");

	while ((length = getline(&line, &capacity, stdin)) >= 0) {
		if (length > 0 && line[length - 1] == '\n')
			length--;
		if (sendto(fd, line, length, 0,
			   (struct sockaddr *)&destination,
			    sizeof(destination)) < 0)
			fail("send");
	}
	if (ferror(stdin))
		fail("stdin");
	free(line);
	close(fd);
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "receive")) {
		run_receiver(argv[2], argv[3]);
	} else if (argc == 6 && !strcmp(argv[1], "send")) {
		run_sender(argv[2], argv[3], argv[4], argv[5]);
	} else {
		fprintf(stderr,
			"Usage: %s receive ADDRESS PORT\n"
			"       %s send ADDRESS PORT SOURCE-ADDRESS DEVICE\n",
			argv[0], argv[0]);
		return 1;
	}
	return 0;
}
