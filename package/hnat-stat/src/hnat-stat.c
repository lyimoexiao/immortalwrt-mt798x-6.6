/*
 * hnat-stat: account traffic offloaded by MediaTek HNAT/WED.
 *
 * Offloaded flows never reach the kernel network stack, so interface
 * counters - and therefore LuCI and Vantage rates - miss them. This daemon
 * keys the bound PPE entries by flow cookie and keeps cumulative byte
 * counts per direction, per LAN client and, when the client MAC is found in
 * a station list, per radio. Readers derive rates from deltas.
 *
 * Read-only: HNAT and WED keep running.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define ENTRY_FILE "/sys/kernel/debug/hnat/all_entry"
#define OUT_FILE "/tmp/hnat_rates.json"
#define OUT_TMP "/tmp/.hnat_rates.json.tmp"
#define MAX_FLOWS 8192
#define MAX_CLIENTS 64
#define MAX_NEIGH 128
#define MAX_STA 128
#define INTERVAL 1
#define MAP_REFRESH 5

enum { RADIO_NONE = 0, RADIO_2G = 1, RADIO_5G = 2 };

struct flow {
	unsigned long long cookie;
	unsigned long long bytes;
	int dir;                /* 0: upload, 1: download */
	char ip[24];
};

struct client {
	char ip[24];
	unsigned long long up, down;
};

struct neigh {
	char ip[24];
	char mac[18];
};

struct sta {
	char mac[18];
	int radio;
};

static struct flow prev_flows[MAX_FLOWS], cur_flows[MAX_FLOWS];
static int prev_count, cur_count;

static struct client clients[MAX_CLIENTS];
static int client_count;

static struct neigh neighs[MAX_NEIGH];
static int neigh_count;

static struct sta stas[MAX_STA];
static int sta_count;

static unsigned long long radio_up[3], radio_dn[3];
static unsigned long long total_up, total_dn;

static unsigned lan_addr, lan_mask;
static int hnat_available = 1;

static unsigned ip_to_u32(const char *s)
{
	unsigned a, b, c, d;

	if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return 0;
	return (a << 24) | (b << 16) | (c << 8) | d;
}

static int is_lan(const char *ip)
{
	unsigned v = ip_to_u32(ip);

	if (!lan_mask || !v)
		return 0;
	return (v & lan_mask) == (lan_addr & lan_mask);
}

static void detect_lan(void)
{
	FILE *f = popen("ip -4 -o addr show br-lan 2>/dev/null | awk '{print $4}' | head -1", "r");
	char buf[64], *slash;

	if (!f)
		return;
	if (fgets(buf, sizeof(buf), f)) {
		slash = strchr(buf, '/');
		if (slash) {
			int bits;

			*slash = 0;
			bits = atoi(slash + 1);
			lan_addr = ip_to_u32(buf);
			lan_mask = bits <= 0 ? 0 :
				   bits >= 32 ? 0xffffffffu :
				   (0xffffffffu << (32 - bits));
		}
	}
	pclose(f);
}

static const char *lookup_mac(const char *ip)
{
	int i;

	for (i = 0; i < neigh_count; i++)
		if (!strcmp(neighs[i].ip, ip))
			return neighs[i].mac;
	return NULL;
}

static int lookup_radio(const char *mac)
{
	int i;

	if (!mac)
		return RADIO_NONE;
	for (i = 0; i < sta_count; i++)
		if (!strcasecmp(stas[i].mac, mac))
			return stas[i].radio;
	return RADIO_NONE;
}

static int client_index(const char *ip)
{
	int i;

	for (i = 0; i < client_count; i++)
		if (!strcmp(clients[i].ip, ip))
			return i;
	if (client_count >= MAX_CLIENTS)
		return -1;
	strncpy(clients[client_count].ip, ip, sizeof(clients[0].ip) - 1);
	clients[client_count].up = 0;
	clients[client_count].down = 0;
	return client_count++;
}

static void add_mac_token(const char *line, int radio)
{
	char *q = (char *)line;
	unsigned v[6];

	while (*q) {
		if (sscanf(q, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
			char mac[18];
			int i;

			snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
				 v[0], v[1], v[2], v[3], v[4], v[5]);
			for (i = 0; i < sta_count; i++)
				if (!strcasecmp(stas[i].mac, mac))
					break;
			if (i == sta_count && sta_count < MAX_STA) {
				strncpy(stas[sta_count].mac, mac, sizeof(stas[0].mac) - 1);
				stas[sta_count].radio = radio;
				sta_count++;
			}
			q += 17;
		} else {
			q++;
		}
	}
}

static void refresh_maps(void)
{
	FILE *f;
	char line[512];

	/* client IP -> MAC, from the bridge neighbour table */
	neigh_count = 0;
	f = popen("ip neigh show dev br-lan 2>/dev/null", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char ip[24], mac[18];

			if (sscanf(line, "%23s lladdr %17s", ip, mac) == 2 && neigh_count < MAX_NEIGH) {
				strncpy(neighs[neigh_count].ip, ip, sizeof(neighs[0].ip) - 1);
				strncpy(neighs[neigh_count].mac, mac, sizeof(neighs[0].mac) - 1);
				neigh_count++;
			}
		}
		pclose(f);
	}

	/* associated stations per radio; iwinfo prints one MAC per line */
	sta_count = 0;
	{
		static const char *cmds[2] = {
			"iwinfo ra0 assoclist 2>/dev/null",
			"iwinfo rax0 assoclist 2>/dev/null",
		};
		int r;

		for (r = 0; r < 2; r++) {
			FILE *s = popen(cmds[r], "r");

			if (!s)
				continue;
			while (fgets(line, sizeof(line), s))
				add_mac_token(line, r + 1);
			pclose(s);
		}
	}
}

static void add_delta(const struct flow *fl, unsigned long long delta)
{
	int ci, radio;

	if (fl->dir)
		total_dn += delta;
	else
		total_up += delta;

	ci = client_index(fl->ip);
	if (ci >= 0) {
		if (fl->dir)
			clients[ci].down += delta;
		else
			clients[ci].up += delta;
	}

	radio = lookup_radio(lookup_mac(fl->ip));
	if (radio != RADIO_NONE) {
		if (fl->dir)
			radio_dn[radio] += delta;
		else
			radio_up[radio] += delta;
	}
}

/*
 * Bound entries look like
 *   ...|type=IPV4_HNAPT|192.168.6.132:55938->118.253.199.2:443=>172.21.1.241:55938->118.253.199.2:443|...
 * for LAN -> WAN and
 *   ...|type=IPV4_HNAPT|118.253.199.2:443->172.21.1.241:55938=>118.253.199.2:443->192.168.6.132:55938|...
 * for WAN -> LAN, so the client sits in the first slot going up and the
 * last slot coming down.
 */
static int parse_tuple(const char *line, char *ip1, char *ip2, char *ip3, char *ip4)
{
	const char *p = strstr(line, "|type=");
	const char *q, *tuple;
	int n;

	if (!p)
		return -1;
	q = strchr(p + 6, '|');
	if (!q)
		return -1;
	tuple = q + 1;

	n = sscanf(tuple, "%23[0-9.]:%*d->%23[0-9.]:%*d=>%23[0-9.]:%*d->%23[0-9.]:%*d",
		   ip1, ip2, ip3, ip4);
	return n == 4 ? 0 : -1;
}

static void scan(void)
{
	FILE *f = fopen(ENTRY_FILE, "r");
	char line[1024];

	cur_count = 0;
	if (!f) {
		hnat_available = 0;
		return;
	}
	hnat_available = 1;

	while (fgets(line, sizeof(line), f)) {
		unsigned long long cookie, bytes;
		char *p;
		char ip1[24], ip2[24], ip3[24], ip4[24];
		int dir, i;

		if (!strstr(line, "state=BIND"))
			continue;
		p = strstr(line, "bytes=");
		if (!p || sscanf(p, "bytes=%llu", &bytes) != 1)
			continue;
		p = strstr(line, "addr=");
		if (!p || sscanf(p, "addr=0x%llx", &cookie) != 1)
			continue;
		if (parse_tuple(line, ip1, ip2, ip3, ip4) != 0)
			continue;

		if (is_lan(ip1)) {
			dir = 0;
		} else if (is_lan(ip4)) {
			dir = 1;
			strncpy(ip1, ip4, sizeof(ip1) - 1);
			ip1[sizeof(ip1) - 1] = 0;
		} else {
			continue;
		}

		for (i = 0; i < prev_count; i++) {
			if (prev_flows[i].cookie == cookie) {
				unsigned long long delta = bytes - prev_flows[i].bytes;

				if (bytes < prev_flows[i].bytes)
					delta = bytes;
				add_delta(&prev_flows[i], delta);
				break;
			}
		}

		if (cur_count < MAX_FLOWS) {
			cur_flows[cur_count].cookie = cookie;
			cur_flows[cur_count].bytes = bytes;
			cur_flows[cur_count].dir = dir;
			strncpy(cur_flows[cur_count].ip, ip1, sizeof(cur_flows[0].ip) - 1);
			cur_count++;
		}
	}
	fclose(f);

	memcpy(prev_flows, cur_flows, sizeof(cur_flows[0]) * cur_count);
	prev_count = cur_count;
}

static void write_json(void)
{
	FILE *f = fopen(OUT_TMP, "w");
	int i;

	if (!f)
		return;
	fprintf(f, "{\"time\":%ld,\"available\":%d,\"up\":%llu,\"down\":%llu,"
		"\"radios\":{\"ra0\":{\"up\":%llu,\"down\":%llu},"
		"\"rax0\":{\"up\":%llu,\"down\":%llu}},\"clients\":{",
		(long)time(NULL), hnat_available, total_up, total_dn,
		radio_up[RADIO_2G], radio_dn[RADIO_2G],
		radio_up[RADIO_5G], radio_dn[RADIO_5G]);
	for (i = 0; i < client_count; i++)
		fprintf(f, "%s\"%s\":{\"up\":%llu,\"down\":%llu}", i ? "," : "",
			clients[i].ip, clients[i].up, clients[i].down);
	fprintf(f, "}}\n");
	fclose(f);
	rename(OUT_TMP, OUT_FILE);
}

int main(void)
{
	int tick = 0;

	detect_lan();

	for (;;) {
		if (tick % MAP_REFRESH == 0)
			refresh_maps();
		scan();
		write_json();
		sleep(INTERVAL);
		tick++;
	}

	return 0;
}