# wifi-led

Single Wi-Fi LED driver for the Netcore N30 Pro.

The board has one Wi-Fi LED (GPIO34) for both radios, and the vendor
`mt_wifi` driver exposes them as `ra0` (2.4 GHz) and `rax0` (5 GHz).

The daemon keeps the LED on the kernel `netdev` trigger with `link=1`,
`tx=1` and `rx=1`, so the LED is solid while a radio is up and blinks in
kernel context on traffic. Because the trigger follows one device, the
daemon samples both radios' counters and repoints `device_name` at
whichever radio moved packets last, every 500 ms.

Offloaded flows (HNAT/WED) do not update the radio counters, so forwarded
traffic is not reflected; only host-terminated traffic blinks the LED.
