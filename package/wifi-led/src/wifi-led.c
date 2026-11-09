/*
 * wifi-led: one LED for both radios, driven by the kernel netdev trigger.
 *
 * link=1 keeps the LED solid while a radio is up, tx/rx=1 make the kernel
 * blink it on traffic. The trigger follows a single device, so this helper
 * points it at whichever radio saw traffic last.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LED_DIR "/sys/class/leds/green:wlan"
#define SAMPLE_US 500000

static const char *ifaces[] = { "ra0", "rax0" };

static void write_value(const char *name, const char *value)
{
	char path[128];
	FILE *f;

	snprintf(path, sizeof(path), LED_DIR "/%s", name);
	f = fopen(path, "w");
	if (!f)
		return;
	fputs(value, f);
	fclose(f);
}

static int read_counter(const char *iface, const char *what, unsigned long long *out)
{
	char path[128];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", iface, what);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%llu", out) != 1)
		*out = 0;
	fclose(f);
	return 0;
}

static void setup_trigger(void)
{
	write_value("trigger", "netdev");
	write_value("link", "1");
	write_value("tx", "1");
	write_value("rx", "1");
}

int main(void)
{
	unsigned long long last[2][2] = { { 0 } };
	int current = -1;

	setup_trigger();

	for (int i = 0; i < 2; i++) {
		read_counter(ifaces[i], "tx_packets", &last[i][0]);
		read_counter(ifaces[i], "rx_packets", &last[i][1]);
	}

	for (;;) {
		unsigned long long tx, rx;
		int active = -1;

		for (int i = 0; i < 2; i++) {
			if (read_counter(ifaces[i], "tx_packets", &tx) ||
			    read_counter(ifaces[i], "rx_packets", &rx))
				continue;
			if (tx != last[i][0] || rx != last[i][1])
				active = i;
			last[i][0] = tx;
			last[i][1] = rx;
		}

		if (active >= 0 && active != current) {
			write_value("device_name", ifaces[active]);
			current = active;
		} else if (current < 0) {
			write_value("device_name", ifaces[0]);
			current = 0;
		}

		usleep(SAMPLE_US);
	}

	return 0;
}
