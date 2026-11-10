/* IP addresses and networks as text. Addresses live in one 128-bit space:
 * an IPv4 address a.b.c.d is ::a.b.c.d (the low 32 bits), as in an IPv6
 * MMDB tree. */
#ifndef NETADDR_H
#define NETADDR_H

#include "common.h"

/* net.ParseIP: dotted IPv4 (no leading zeros) or IPv6. An IPv4-mapped IPv6
 * address (::ffff:a.b.c.d) counts as IPv4, as Go's To4 does. */
bool parse_ip_text(const char *s, size_t n, u128 *out, bool *is4);

/* One network cell: "1.2.3.0/24", "2001:db8::/32", a single address, a
 * range "1.2.3.0-1.2.3.255" (spaces allowed around the dash), or a decimal
 * integer (IPv4 up to 4294967295, IPv6 above). CIDR host bits are cleared,
 * as net.ParseCIDR does. *is4 is true when the network is IPv4. */
bool parse_network(const char *s, size_t n, u128 *start, u128 *end, bool *is4);
/* An address of a start or end column: an IP or a decimal integer. */
bool parse_address(const char *s, size_t n, u128 *out, bool *is4);

/* net.IPNet.String for a tree network: IPv4 when it lies in ::/96 with a
 * prefix of at least 96 bits (as maxminddb-golang returns them), else IPv6.
 * bits is the prefix length in the 128-bit space. */
const char *fmt_net(u128 ip, int bits, char out[80]);
/* net.IP.String of an address given as IPv4 or not. */
const char *fmt_ip(u128 ip, bool is4, char out[64]);

/* The last address of the network ip/bits (bits in the 128-bit space). */
static inline u128 net_last(u128 ip, int bits) { return bits >= 128 ? ip : ip | (U128_MAX >> bits); }

/* Calls fn with the fewest CIDRs that cover [start, end], in order. bits are
 * prefix lengths in the 128-bit space. */
void range_cidrs(u128 start, u128 end, void (*fn)(void *ctx, u128 ip, int bits), void *ctx);

#endif
