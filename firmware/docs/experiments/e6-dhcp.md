# E6 -- What the DHCP exchange is made of

[Experiment index](../experiments.md)

**Status and conclusion.** Taken 2026-09-17. DHCP includes about 2.1 s waiting for OFFER and 1 s of ARP checking. Caching the lease avoids both; the rig connected in 178–226 ms, and integrated firmware in 302–307 ms from setup.

The record below preserves the conditions and decisions at the time of the
measurement. S-numbers identify historical implementation stages; D4/D6 were
resolved by streaming and partial refresh. Current behavior is described in
[implementation.md](../implementation.md). Commands and plain `src/` / `tools/`
paths are relative to `firmware/`.

**Why.** [S6](#firmware-network-validation) measured a connect on the home
network and found the association is nothing and the address is everything: the
link comes up 77-158 ms after `WiFi.begin()` and the IP arrives 3.14-3.24 s
later, over thirty times as long. Every question pays it between the release and
the upload, so it is the largest single number in the flow that is not the
panel -- and the BSSID cache the vision asks for, which does work, saves about
100 ms of it.

The figure is also too stable to be a busy server: six consecutive wakes came
back within 100 ms of each other. That is the shape of a fixed timer, and a
timer is something firmware can usually stop waiting for.

**What to measure.** Where the 3.15 s actually goes, and what each candidate
removes:

1. The exchange itself -- DISCOVER, OFFER, REQUEST, ACK -- timed from the
   station's side, which needs either a capture on the network or the lwIP DHCP
   client's own state transitions.
2. lwIP's ARP check on the offered address. `CONFIG_LWIP_DHCP_DOES_ARP_CHECK` is
   on by default in ESP-IDF and costs a probe plus a wait before the address is
   handed over. It cannot be turned off from `framework = arduino` without
   moving to `arduino, espidf`, so its share is worth knowing before anyone
   pays that price.
3. The address kept in RTC memory across the sleep and set with `WiFi.config()`
   before `begin()`, which skips the client altogether. This is the cheap
   candidate and the one most likely to work; it needs a fallback to DHCP when
   the lease has gone stale, and a rule for what "stale" means.

**How.** `src/experiments/e6_dhcp_cost.cpp`:

```
~/.platformio/penv/bin/pio run -e exp_e6 -t upload --upload-port <port>
```

It associates through `WifiLink`, so the association half is the firmware's
own code, and alternates wake by wake: an ordinary DHCP wake, then one that
installs the previous wake's lease with `WiFi.config()`. On DHCP wakes the lwIP
client's state machine is sampled every 2 ms through `netif_dhcp_data()`, which
is where the timeline comes from. Every fourth wake installs a lease from a
different subnet on purpose, to measure what a stale one costs.

**An address nobody used is not an address**, so each wake ends with a TCP
connect to the gateway. A refusal proves as much as an acceptance -- something
replied to this address -- and only a timeout means the address does not work.

**Already ruled out.** WiFi power save: `WiFi.setSleep(false)` before the
association changed nothing at all (3.11-3.15 s against 3.14-3.24 s), so the
station is not asleep through an answer that has already arrived. Measured, no
change needed, and the line was taken back out.

**Watch for.** A fixed address is a claim on someone else's network, so a device
that keeps one has to notice when the network disagrees -- another client on the
same address, or a different network entirely behind the same SSID. The safe
version is a cached lease that is tried first and dropped the moment anything
about it fails, which is the same shape as the BSSID cache S6 already has.

## Result

Twelve wakes on the home network, alternating, with every fourth one carrying a
lease from a subnet the device was not on.

| Wake | Address | Probe | Top of setup() to a usable network |
| --- | --- | --- | --- |
| DHCP | 3166-3293 ms | 12-15 ms | 3268-3427 ms |
| cached lease | 39-90 ms | 32-34 ms | 178-226 ms |
| stale lease | 39-44 ms, then 3137-3225 ms of DHCP | 3019-3033 ms of timeouts | 6323-6424 ms |

**The 3.2 s is two things, and only one of them is ours.** The timeline is the
same on every DHCP wake:

```
      44 ms  SELECTING    DISCOVER is out
    2129 ms  REQUESTING   an OFFER came back, REQUEST is out
    2166 ms  CHECKING     the ACK is in, ARP-checking the address
    3166 ms  BOUND        the address is ours
   0 retries, lease 86400 s
```

- **2.1 s of it is the router taking its time over the OFFER**, and it is not a
  lost packet: `tries` is zero on every wake, so nothing was retransmitted. The
  same server then answers the REQUEST with an ACK in 30-50 ms, so it is not a
  slow server in general -- only a slow first answer. Nothing on the device can
  make that faster.
- **1.000 s of it is the ARP check**, to the millisecond, on every wake.
  `CONFIG_LWIP_DHCP_DOES_ARP_CHECK=y` in the prebuilt libraries this project
  links against, and lwipopts.h says the check "lasts 1 - 2 seconds". Turning it
  off means `framework = arduino, espidf` and a menuconfig, which is a large
  price for a third of the number.

**A cached lease removes all of it.** `WiFi.config()` with the previous wake's
address, installed before the association, gets the device onto a network it can
use in 178-226 ms from the top of `setup()` -- against 3268-3427 ms for DHCP,
which is the same connect fifteen times over. The lease this network hands out
is 86400 s, a day, so an address is good for far longer than the RTC memory
holding it survives.

**A stale lease costs one question, not a failure.** An address from the wrong
subnet installs happily and then answers nothing: both probes time out, and
asking properly afterwards takes the usual 3.2 s, for 6.3-6.4 s in total. That
is the whole risk, and it is bounded -- twice a normal connect on the wake that
notices, and back to 180 ms afterwards.

**What it changes.** Caching the lease is worth roughly three seconds of every
question and is the largest single saving available anywhere in the flow. Two
things make it cheaper than it looks in the firmware:

- **The upload is already the probe.** S7 opens a TCP connection to the backend
  on every question, so a cached address that has gone stale shows up as a
  connect that fails -- no extra probe is needed on the happy path, and the
  fallback is to drop the lease, start DHCP and try again.
- **The lease has a known life.** 86400 s from the server, and the RTC counter
  survives deep sleep, so an entry can be aged out rather than trusted forever.

**The firmware now carries it, and the numbers held.**
[S7b](#firmware-network-validation) is this result as a step:
3546 ms from the top of `setup()` to a usable network on a DHCP wake against
302-307 ms on a cached one, measured on the firmware rather than on this rig, for
a saving of 3.24 s a question. Two corrections came back with it:

- **The rig's 178-226 ms and the firmware's 302-307 ms are the same number.**
  The difference is the 214-228 ms of microphone, buffer, capture task and ready
  chirp the firmware does before `WiFi.begin()` and the rig did not.
- **A stale lease costs 13.2 s in the firmware, not 6.3 s.** The rule the
  firmware settled on drops the lease on the *second* connect that answers
  nothing rather than the first, so the probe half is paid twice: 10007 ms of
  connects plus 3151 ms of DHCP. The extra 5 s buys not throwing away a working
  address on a connect that failed for its own reasons.

**A trap found on the way.** `WiFi.persistent(false)` has to be called *before*
`WiFi.mode(WIFI_STA)`. Arduino's default storage is FLASH, so a mode set while
that is still true goes through NVS and costs 1.6 s -- a full half of a cached
connect's entire budget, spent before the association even starts. With the
order right, `WiFi.mode()` takes 33-48 ms. `WifiLink::begin()` already does it
in that order; the first version of this rig did not, which is how it was
found.

## Firmware network validation

These are historical integrated-firmware observations (S6 and S7b), distinct
from the isolated rig above. They explain the cache/fallback policy; do not
read them as current-network guarantees.

Before lease caching, eleven wakes included a router shutdown and restart:

| Wake | Path | Link | Online | Boot to online |
| --- | --- | --- | --- | --- |
| 1, cold | scan | 101 ms | 3301 ms | 3454 ms |
| 2-7 | cached | 77-97 ms | 3208-3318 ms | 3261-3371 ms |
| 8, router off | cached, then two scans | -- | `NO WIFI` at 15005 ms | 15056 ms |
| 9, router back | scan | 1298 ms | 4408 ms | 4461 ms |
| 10-11 | cached | 78-88 ms | 3208-3258 ms | 3261-3311 ms |

The router returned on channel 11 instead of 1; scanning found it and subsequent
cached wakes were fast again. Disabling WiFi power save over six wakes did not
remove the 3.11–3.24 s address delay. Router-off failure at 15005 ms matched the
15-second budget (3 seconds cached AP, then two 6-second scans).

After lease caching, the corrected second run of six wakes measured:

| Mode | Link | Address | Top of `setup()` to a usable network |
| --- | --- | --- | --- |
| DHCP | 85 ms | 3332 ms | 3546 ms |
| cached lease | 78-83 ms | the same millisecond | 302-307 ms |
| stale lease | 78 ms | the same millisecond | 302 ms, and then 13.2 s |

The first run's poll-based timestamps falsely reported 2641–2644 ms for cached
network readiness because the orchestrator was drawing LISTENING. Link/address
timestamps must come from the WiFi task. Firmware startup before `wifi.begin()`
accounts for the difference from the rig's 178–226 ms.

The cache read an 86400-second lease and aged it across deep sleeps. A closed
port refused in 58 ms and retained the lease. A deliberately stale lease
recovered after two unanswered connects (10007 ms), DHCP (3151 ms), and a fresh
request: about 13.2 seconds. Retrying once before invalidation avoids discarding
a good address for a transient connect failure; a false positive still pays
that cost. Streaming later moved this recovery under sufficiently long holds
([E7](e7-upload.md#firmware-streaming-validation)).
