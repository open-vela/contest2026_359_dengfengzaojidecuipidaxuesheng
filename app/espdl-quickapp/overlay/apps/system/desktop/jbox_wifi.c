/* SPDX-License-Identifier: Apache-2.0 */
#include "jbox_wifi.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>

struct field {
  unsigned tag, wire;
  uint32_t value;
  const uint8_t *bytes;
  size_t length;
};
static void wipe(void *p, size_t n) {
  volatile uint8_t *v = p;
  while (n--)
    *v++ = 0;
}
static int varint(const uint8_t **p, const uint8_t *end, uint32_t *v) {
  *v = 0;
  for (unsigned i = 0; i < 5; i++) {
    if (*p == end)
      return -EPROTO;
    uint8_t b = *(*p)++;
    if (i == 4 && (b & 0xf0))
      return -EPROTO;
    *v |= (uint32_t)(b & 127) << (7 * i);
    if (!(b & 128))
      return 0;
  }
  return -EPROTO;
}
/* Bound every field before reading it; duplicates and unsupported wire types
 * are rejected instead of accepting an ambiguous protobuf oneof. */
static int find(const uint8_t *p, size_t n, unsigned tag, struct field *out) {
  const uint8_t *end = p + n;
  bool found = false;
  memset(out, 0, sizeof(*out));
  while (p < end) {
    uint32_t key = 0, length = 0;
    struct field f = {0};
    if (varint(&p, end, &key) || !(key >> 3))
      return -EPROTO;
    f.tag = key >> 3;
    f.wire = key & 7;
    if (!f.wire) {
      if (varint(&p, end, &f.value))
        return -EPROTO;
    } else if (f.wire == 2) {
      if (varint(&p, end, &length) || length > (size_t)(end - p))
        return -EPROTO;
      f.bytes = p;
      f.length = length;
      p += length;
    } else
      return -EPROTO;
    if (f.tag == tag) {
      if (found)
        return -EPROTO;
      *out = f;
      found = true;
    }
  }
  return found ? 0 : -ENOENT;
}
static int number(const uint8_t *p, size_t n, unsigned tag, uint32_t expected) {
  struct field f;
  int r = find(p, n, tag, &f);
  if (r == -ENOENT)
    return expected == 0 ? 0 : -EPROTO;
  return r || f.wire || f.value != expected ? -EPROTO : 0;
}
static size_t put_var(uint8_t *p, uint32_t v) {
  size_t n = 0;
  do {
    p[n] = (uint8_t)(v & 127);
    v >>= 7;
    if (v)
      p[n] |= 128;
    n++;
  } while (v);
  return n;
}
static size_t put_bytes(uint8_t *p, unsigned tag, const uint8_t *data,
                        size_t n) {
  size_t k = put_var(p, (tag << 3) | 2);
  k += put_var(p + k, (uint32_t)n);
  if (n)
    memcpy(p + k, data, n);
  return k + n;
}
static int payload(const uint8_t *p, size_t n, unsigned type, unsigned tag,
                   struct field *f) {
  if (number(p, n, 1, type) || find(p, n, tag, f) || f->wire != 2)
    return -EPROTO;
  /* Reject an unexpected second message in this protobuf oneof. */
  for (unsigned t = 10; t <= 23; t++)
    if (t != tag) {
      struct field other;
      int r = find(p, n, t, &other);
      if (!r)
        return -EPROTO;
      if (r != -ENOENT)
        return r;
    }
  return 0;
}
static int session_reply(const uint8_t *p, size_t n, unsigned type,
                         unsigned tag, struct field *f) {
  struct field sec;
  if (number(p, n, 2, 1) || find(p, n, 11, &sec) || sec.wire != 2)
    return -EPROTO;
  for (unsigned t = 10; t <= 12; t += 2) {
    struct field other;
    if (find(p, n, t, &other) != -ENOENT)
      return -EPROTO;
  }
  return payload(sec.bytes, sec.length, type, tag, f);
}
static size_t session_request(uint8_t *out, unsigned type, unsigned tag,
                              unsigned key, const uint8_t data[32]) {
  uint8_t body[40], sec[48];
  size_t b = put_bytes(body, key, data, 32), n = 0;
  if (type) {
    sec[n++] = 8;
    sec[n++] = (uint8_t)type;
  }
  n += put_bytes(sec + n, tag, body, b);
  out[0] = 16;
  out[1] = 1;
  return 2 + put_bytes(out + 2, 11, sec, n);
}
static int exchange(const struct jbox_wifi_ops *o, int session,
                    uint8_t *request, size_t n, uint8_t response[192],
                    size_t *received) {
  int r = o->check(o->context);
  *received = 192;
  if (r)
    return r;
  r = o->exchange(o->context, session, request, n, response, received);
  if (r)
    return r;
  if (*received > 192)
    return -EMSGSIZE;
  return o->check(o->context);
}
static int config(const struct jbox_wifi_ops *o, unsigned type,
                  unsigned req_tag, unsigned resp_tag, const uint8_t *body,
                  size_t length, uint8_t plain[192], size_t *n) {
  uint8_t request[192] = {0}, reply[192] = {0};
  size_t k = 0, received = 0;
  struct field f;
  int r;
  if (type) {
    request[k++] = 8;
    request[k++] = (uint8_t)type;
  }
  k += put_bytes(request + k, req_tag, body, length);
  r = o->crypt(o->context, request, k, plain);
  if (!r)
    r = exchange(o, 0, plain, k, reply, &received);
  if (!r)
    r = o->crypt(o->context, reply, received, plain);
  if (!r)
    r = payload(plain, received, type + 1, resp_tag, &f);
  if (!r)
    r = number(f.bytes, f.length, 1, 0);
  if (!r) {
    memmove(plain, f.bytes, f.length);
    *n = f.length;
  }
  wipe(request, sizeof(request));
  wipe(reply, sizeof(reply));
  return r;
}
static bool ipv4(const uint8_t *s, size_t n) {
  if (!n || n > 15)
    return false;
  unsigned parts = 0, value = 0, digits = 0, first = 0;
  for (size_t i = 0; i <= n; i++) {
    unsigned c = i < n ? s[i] : '.';
    if (c >= '0' && c <= '9') {
      if (++digits > 3)
        return false;
      value = value * 10 + c - '0';
      if (value > 255)
        return false;
    } else if (c == '.') {
      if (!digits || ++parts > 4)
        return false;
      if (parts == 1)
        first = value;
      value = digits = 0;
    } else
      return false;
  }
  return parts == 4 && first > 0 && first < 224 && first != 127;
}
int jbox_wifi_provision(const struct jbox_wifi_ops *o, const char *ssid,
                        const char *password, char ip[16]) {
  if (!o || !ssid || !password || !ip || !o->check || !o->keypair ||
      !o->cipher_start || !o->crypt || !o->exchange || !o->pause)
    return -EINVAL;
  ip[0] = 0;
  size_t sl = strnlen(ssid, 33), pl = strnlen(password, 65);
  if (!sl || sl > 32 || pl > 64)
    return -EINVAL;
  uint8_t public_key[32] = {0}, peer[32] = {0}, request[192] = {0},
          response[192] = {0}, plain[192] = {0};
  struct field body, key, iv, proof, state, connected, address;
  size_t n = 0, k = 0;
  int r = o->check(o->context);
  if (!r)
    r = o->keypair(o->context, public_key);
  if (r)
    goto done;
  k = session_request(request, 0, 20, 1, public_key);
  r = exchange(o, 1, request, k, response, &n);
  if (r)
    goto done;
  if (session_reply(response, n, 1, 21, &body) ||
      number(body.bytes, body.length, 1, 0) ||
      find(body.bytes, body.length, 2, &key) || key.wire != 2 ||
      key.length != 32 || find(body.bytes, body.length, 3, &iv) ||
      iv.wire != 2 || iv.length != 16) {
    r = -EPROTO;
    goto done;
  }
  memcpy(peer, key.bytes, 32);
  r = o->cipher_start(o->context, peer, iv.bytes);
  if (r)
    goto done;
  r = o->crypt(o->context, peer, 32, plain);
  if (r)
    goto done;
  k = session_request(request, 2, 22, 2, plain);
  r = exchange(o, 1, request, k, response, &n);
  if (r)
    goto done;
  if (session_reply(response, n, 3, 23, &body) ||
      number(body.bytes, body.length, 1, 0) ||
      find(body.bytes, body.length, 3, &proof) || proof.wire != 2 ||
      proof.length != 32) {
    r = -EPROTO;
    goto done;
  }
  r = o->crypt(o->context, proof.bytes, 32, plain);
  if (r)
    goto done;
  unsigned diff = 0;
  for (unsigned i = 0; i < 32; i++)
    diff |= plain[i] ^ public_key[i];
  if (diff) {
    r = -EACCES;
    goto done;
  }
  /* This is the only credential write and the only apply in this attempt. */
  k = put_bytes(request, 1, (const uint8_t *)ssid, sl);
  k += put_bytes(request + k, 2, (const uint8_t *)password, pl);
  r = config(o, 2, 12, 13, request, k, plain, &n);
  wipe(request, sizeof(request));
  if (r)
    goto done;
  r = config(o, 4, 14, 15, NULL, 0, plain, &n);
  if (r)
    goto done;
  for (unsigned poll = 0; poll < 90; poll++) {
    r = config(o, 0, 10, 11, NULL, 0, plain, &n);
    if (r)
      goto done;
    r = find(plain, n, 2, &state);
    if (r == -ENOENT) {
      memset(&state, 0, sizeof(state));
      r = 0;
    }
    if (r || state.wire || state.value > 3) {
      r = -EPROTO;
      goto done;
    }
    if (state.value == 0) {
      if (find(plain, n, 11, &connected) || connected.wire != 2 ||
          find(connected.bytes, connected.length, 1, &address) ||
          address.wire != 2 || !ipv4(address.bytes, address.length)) {
        r = -EPROTO;
        goto done;
      }
      memcpy(ip, address.bytes, address.length);
      ip[address.length] = 0;
      r = 0;
      goto done;
    }
    if (state.value >= 2) {
      r = -ENETUNREACH;
      goto done;
    }
    r = o->pause(o->context, 500);
    if (r)
      goto done;
  }
  r = -ETIMEDOUT;
done:
  wipe(public_key, sizeof(public_key));
  wipe(peer, sizeof(peer));
  wipe(request, sizeof(request));
  wipe(response, sizeof(response));
  wipe(plain, sizeof(plain));
  return r;
}
