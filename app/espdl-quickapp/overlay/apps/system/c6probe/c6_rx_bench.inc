/* SPDX-License-Identifier: Apache-2.0 */
/* Local, bounded TCP receive diagnostic. Payload is never saved or printed. */

#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <nuttx/crc32.h>

static int64_t c6_bench_ms(void)
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int c6_bench_wait(int fd, short events, int64_t deadline)
{
  struct pollfd pfd = {fd, events, 0};
  int ret;
  int64_t left;

  for (;;)
    {
      int64_t now = c6_bench_ms();
      if (now < 0) return -EIO;
      left = deadline - now;
      if (left <= 0) return -ETIMEDOUT;
      ret = poll(&pfd, 1, left > 5000 ? 5000 : (int)left);
      if (ret < 0 && errno == EINTR) continue;
      if (ret < 0) return -errno;
      if (ret == 0) return -ETIMEDOUT;
      if (pfd.revents & (events | POLLHUP)) return 0;
      return -EIO;
    }
}

static int c6_rx_bench_command(int argc, FAR char **argv)
{
  struct sockaddr_in address = {0};
  FAR uint8_t *buffer = NULL;
  FAR char *end;
  unsigned long port;
  unsigned long bytes;
  unsigned long expected;
  uint32_t received = 0;
  uint32_t crc = UINT32_MAX;
  int64_t started;
  int64_t elapsed;
  int64_t deadline;
  int fd = -1;
  int ret = -EINVAL;

  if (argc != 6 || strlen(argv[5]) != 8) return 1;
  for (unsigned int n = 0; n < 8; n++)
    {
      unsigned char c = argv[5][n];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F'))) return 1;
    }
  port = strtoul(argv[3], &end, 10);
  if (!argv[3][0] || *end || port == 0 || port > 65535) return 1;
  bytes = strtoul(argv[4], &end, 10);
  if (!argv[4][0] || *end || bytes == 0 || bytes > 16u * 1024u * 1024u)
    return 1;
  expected = strtoul(argv[5], &end, 16);
  if (*end || expected > UINT32_MAX) return 1;
  if (inet_pton(AF_INET, argv[2], &address.sin_addr) != 1) return 1;
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)port);
  started = c6_bench_ms();
  if (started < 0) return 1;
  deadline = started + 60000;
  buffer = malloc(4096);
  if (!buffer) { ret = -ENOMEM; goto done; }
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { ret = -errno; goto done; }
  ret = fcntl(fd, F_GETFL, 0);
  if (ret < 0 || fcntl(fd, F_SETFL, ret | O_NONBLOCK) < 0)
    { ret = -errno; goto done; }
  ret = connect(fd, (FAR struct sockaddr *)&address, sizeof(address));
  if (ret < 0)
    {
      int error;
      socklen_t length = sizeof(error);
      if (errno != EINPROGRESS) { ret = -errno; goto done; }
      ret = c6_bench_wait(fd, POLLOUT, started + 5000);
      if (ret < 0) goto done;
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
        { ret = -errno; goto done; }
      if (error) { ret = -error; goto done; }
    }

  while (received < bytes)
    {
      size_t wanted = bytes - received;
      ssize_t n;
      if (wanted > 4096) wanted = 4096;
      ret = c6_bench_wait(fd, POLLIN, deadline);
      if (ret < 0) goto done;
      n = recv(fd, buffer, wanted, 0);
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      if (n <= 0) { ret = n < 0 ? -errno : -EIO; goto done; }
      crc = crc32part(buffer, (size_t)n, crc);
      received += n;
    }

  /* Exact length and orderly EOF are required, not merely a matching prefix. */

  ret = c6_bench_wait(fd, POLLIN, deadline);
  if (ret < 0) goto done;
  ret = recv(fd, buffer, 1, 0);
  if (ret != 0) { ret = ret < 0 ? -errno : -EMSGSIZE; goto done; }
  crc ^= UINT32_MAX;
  ret = crc == (uint32_t)expected ? 0 : -EBADMSG;

done:
  elapsed = c6_bench_ms() - started;
  if (elapsed <= 0 && ret == 0) ret = -EIO;
  if (fd >= 0) close(fd);
  free(buffer);
  printf("RXBENCH result=%d bytes=%" PRIu32 " crc=%08" PRIx32
         " elapsed_ms=%" PRId64 "\n", ret, received, crc, elapsed);
  return ret == 0 ? 0 : 1;
}
