/*
 * hnat-stat: account traffic offloaded by MediaTek HNAT/WED.
 *
 * Offloaded flows never reach the kernel network stack, so interface
 * counters miss them. This daemon keys PPE bind entries by their flow
 * cookie and keeps cumulative byte counts per direction, per LAN client
 * and, for clients associated on a radio, per radio. Readers derive rates
 * from deltas of these cumulative values.
 *
 * It is read-only: HNAT and WED keep running unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define RADIO_REFRESH 10

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
	for (i = 0; i < sta_count; i++) {
		const char *a = stas[i].mac, *b = mac;
		int eq = 1;

		while (*a && *b) {
			char ca = (*a >= 'A' && *a <= 'F') ? *a + 32 : *a;
			char cb = (*b >= 'A' && *b <= 'F') ? *b + 32 : *b;

			if (ca != cb) {
				eq = 0;
				break;
			}
			a++; b++;
		}
		if (eq && !*a && !*b)
			return stas[i].radio;
	}
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
			int i, dup = 0;

			snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
				 v[0], v[1], v[2], v[3], v[4], v[5]);
			for (i = 0; i < sta_count; i++)
				if (!strcmp(stas[i].mac, mac)) {
					dup = 1;
					break;
				}
			if (!dup && sta_count < MAX_STA && radio) {
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

	sta_count = 0;
	{
		static const char *cmds[2] = {
			"iwpriv ra0 show stainfo 2>/dev/null",
			"iwpriv rax0 show stainfo 2>/dev/null",
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
		char *p, *q, *tuple, *arrow;
		char first[64], second[64], ip1[24];
		int dir, i;

		if (!strstr(line, "state=BIND"))
			continue;
		p = strstr(line, "bytes=");
		if (!p || sscanf(p, "bytes=%llu", &bytes) != 1)
			continue;
		p = strstr(line, "addr=");
		if (!p || sscanf(p, "addr=0x%llx", &cookie) != 1)
			continue;
		p = strstr(line, "|type=");
		if (!p)
			continue;
		q = strstr(p + 6, "|");
		if (!q)
			continue;
		tuple = q + 1;
		arrow = strstr(tuple, "=>");
		if (!arrow)
			continue;
		*arrow = 0;

		if (sscanf(tuple, "%23[^:]:%*d->", first) != 1)
			continue;
		if (sscanf(arrow + 2, "%23[^:]:%*d->", second) != 1)
			continue;

		if (is_lan(first)) {
			dir = 0;
			strncpy(ip1, first, sizeof(ip1) - 1);
		} else if (is_lan(second)) {
			dir = 1;
			strncpy(ip1, second, sizeof(ip1) - 1);
		} else {
			continue;
		}
		ip1[sizeof(ip1) - 1] = 0;

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
		if (tick % RADIO_REFRESH == 0)
			refresh_maps();
		scan();
		write_json();
		sleep(INTERVAL);
		tick++;
	}

	return 0;
}
