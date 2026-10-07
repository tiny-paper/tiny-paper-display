/*
 * url_host.h - the host an HTTP client connects to, and private IPv4 ranges, for hal_http's public_only check
 * (esp/net.c). Hardware-free, host-tested. The authority ends at the first '/', '?' or '#', as in esp_http_client
 * (http_parser): a URL like http://10.0.0.1?@example.com/ connects to 10.0.0.1, so any '@' in it is refused.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_URL_HOST_H
#define TINY_URL_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* http(s) URL -> host (no port) in host[cap]. false: no scheme, empty host, userinfo ('@'), IPv6 literal, too long. */
bool url_host(const char *url, char *host, size_t cap);

/* a = the 4 address bytes in network order: loopback, private, link-local, CGNAT, reserved, multicast. */
bool url_addr_private(const uint8_t a[4]);

#endif
