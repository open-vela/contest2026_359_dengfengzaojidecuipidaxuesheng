/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

int buzzos_secure_random(void *output, size_t length)
{
	if (!length) return 0;
	if (!output) return -1;
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) return -1;
	uint8_t *bytes = output;
	while (length) {
		size_t chunk = length > 4096 ? 4096 : length;
		ssize_t n = read(fd, bytes, chunk);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { close(fd); return -1; }
		bytes += n; length -= n;
	}
	close(fd); return 0;
}
