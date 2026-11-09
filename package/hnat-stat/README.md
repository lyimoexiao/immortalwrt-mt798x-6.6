# hnat-stat

Traffic accounting for flows offloaded by the MediaTek HNAT/PPE engine.

Offloaded flows bypass the kernel network stack, so `/proc/net/dev`, ubus
`network.device` and therefore LuCI/Vantage rates miss them. This daemon
reads `/sys/kernel/debug/hnat/all_entry`, keys each bound flow by its
cookie and keeps cumulative byte counters:

- total upload/download,
- per LAN client (`clients.<ip>`),
- per radio (`radios.ra0`, `radios.rax0`), when the client MAC is found in
  `iwpriv <if> show stainfo`.

Counters are written atomically to `/tmp/hnat_rates.json` once a second,
and exposed to LuCI through the `hnat` ubus object (`hnat rates`). Readers
derive rates from deltas of the cumulative values.

It is read-only: HNAT and WED stay enabled. Per-radio attribution depends
on the neighbour table and the station lists, so it is approximate.
