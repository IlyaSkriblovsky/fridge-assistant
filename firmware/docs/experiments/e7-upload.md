# E7 -- What the upload costs

[Experiment index](../experiments.md)

**Status and conclusion.** Taken 2026-09-17; streaming verified 2026-09-18. Burst uploads stalled on lost acknowledgements; streaming moved the upload under the hold. With the fixed-response prototype, release to answer chirp was 115–154 ms, not a model-response latency guarantee.

The record below preserves the conditions and decisions at the time of the
measurement. S-numbers identify historical implementation stages; D4/D6 were
resolved by streaming and partial refresh. Current behavior is described in
[implementation.md](../implementation.md). Commands and plain `src/` / `tools/`
paths are relative to `firmware/`.

**Why.** [S8](e8-refresh.md#firmware-display-validation) has to decide what the device
shows between the release and the answer, and the number that decides it is how
long the round trip actually takes.
[S7](#firmware-upload-validation) measured it over nine questions
and the answer was "between 260 ms and 7.4 s", which is not a number a decision
can be taken on.

The spread is not the backend: it answers an empty POST in 4 ms and swallows
120 KB in the same 4 ms, measured over the LAN. It is not the payload either --
two 34 KB recordings took 260 ms and 3319 ms.

**What it looks like.** Against the fastest rate seen, 145 KB/s, four of the
nine questions cost what their size says they should and the other five carry a
penalty of 1.1, 2.4, 2.6, 3.1 and 5.1 seconds. Penalties near whole seconds are
what TCP retransmission timeouts look like -- 1 s, then 2 s, then 3 s -- so the
first hypothesis is that packets are being lost at the front of the connection
rather than that the link is slow. It is a hypothesis and nothing more: nine
samples taken while walking an error table are not a measurement of throughput.

**What to measure.**

- The connect, the body write and the wait for the status line, apart.
  `HTTPClient` reports only their sum, which is why S7's number is one number;
  a bare `NetworkClient` separates them.
- The same upload with `WiFi.setSleep(false)`. [E6](e6-dhcp.md#e6----what-the-dhcp-exchange-is-made-of)
  measured power save against the DHCP exchange and found nothing, but that is
  two round trips. An upload is hundreds of ACKs, which is where power save is
  supposed to cost, and a station that sleeps between beacons is one of the few
  things that would delay a packet by whole seconds.
- The same upload seen from the backend, with `tcpdump`. A retransmit is only
  visible as a retransmit from a machine that can see both copies.
- **The connects that never arrive.** Four of S7's questions could not open a
  connection to a backend that was running and gave up at the 5 s timeout, with
  nothing reaching the backend at all and the wakes on either side answered
  normally. It is very likely the same thing as the penalty above, seen at the
  handshake instead of in the body, and the same capture settles it. It has to
  be settled: [S7b](e6-dhcp.md#firmware-network-validation) reads a failed
  connect as a stale lease, and one false reading every fifteen questions would
  have it throwing away an address that was fine.

**How.** `src/experiments/e7_upload_cost.cpp`:

```
~/.platformio/penv/bin/pio run -e exp_e7 -t upload --upload-port <port>
```

Sixteen wakes, four uploads each, all of them the same 128044-byte payload --
`Recording`'s own buffer filled with a tone instead of by the microphone, so
the body is the firmware's allocation and not the rig's invention. Uploads 1 to
3 go through a bare `NetworkClient` at 0, +3 and +6 s after the address arrives,
with the body written in 4 KB chunks so a stall has an offset as well as a
duration. Upload 4 goes through `Backend` unchanged, which anchors the
rig's six clocks to the one number S7 reported. `WiFi.setSleep(false)` on even
wakes.

The capture is taken on the backend, which is the only place both directions are
visible:

```
sudo tcpdump -i en0 -n -s 128 -w /tmp/e7.pcap 'tcp port 8000'
tools/e7_pcap.py /tmp/e7.pcap        # every connection, one line each
tools/e7_pcap.py /tmp/e7.pcap 32     # every packet of connection 32
```

`-s 128` keeps the headers and throws the data away, so the payload length has
to be read out of the IP header rather than counted -- which is the first thing
`tools/e7_pcap.py` gets right and the reason it exists. tshark is not on this
machine and scapy is not in the backend's venv, so the pcap is parsed by hand.

**Watch for.** The penalty has to be separated from the association it follows.
Every question here uploads within a second or two of the address arriving, so
a cost that belongs to a link that has just come up would look like a cost of
the upload. Uploading twice per wake, a few seconds apart, answers that in one
run.

**The rig cannot be watched with `pio device monitor`:** it needs a terminal on
stdin, which an unattended run does not have.
`tools/serial_log.py` reads the port with pyserial instead, and buys something
the monitor does not give -- a host timestamp per line, so the log and the
capture are on one axis:

```
tools/serial_log.py /dev/cu.usbmodem<...> e7-serial.log
```

**What it unblocks.** The working screen at S8, and [D6](../deferred.md) with it:
2.4 s of partial refresh is worth arguing about against a 300 ms round trip and
is noise against a 7 s one. Also [D4](../deferred.md) -- a streaming upload is
worth much more if the cost is per packet than if it is a stall at the front --
and [E2](e2-https-overhead.md#e2----https-overhead), which cannot compare HTTPS against HTTP while
HTTP varies by a factor of ten by itself.

## Result

Sixty-four uploads over sixteen wakes, 7.85 MB up, every one of them answered
200. The phases, over the 48 that went through the bare socket:

| | min | median | max |
| --- | --- | --- | --- |
| connect, upload 1 of a wake | 48 ms | 142 ms | 944 ms |
| connect, uploads 2 and 3 | 7 ms | 15 ms | 1099 ms |
| header | 1 ms | 1 ms | 1 ms |
| body, no stall (35 of 48) | 472 ms | 617 ms | 2742 ms |
| body, with a stall (13 of 48) | 1519 ms | 2067 ms | 5352 ms |
| the backend's own answer | 6 ms | 15 ms | 151 ms |

**128 KB goes up in 660 ms and the backend is not in it.** A clean upload runs
at 194 KB/s and the wait for the status line is 15 ms, which is the LAN figure
S7 already had. Everything S7 could not explain is in the other column.

**The penalty is one chunk out of thirty-two.** A stalled body is a normal body
with a hole in it: take the worst chunk out and what is left is 484/660/2037 ms,
which is the clean column again. The stalls themselves are 1023, 1035, 1047,
1067, 1084, 1291, 1303, 1377, 1472, 1476, 2513, 2553 and 4243 ms -- clustered at
one second and at two and a half, which is what a retransmission timeout and its
first doubling look like.

**Nothing is lost on the way up.** Across 71 connections and some 6700 segments
the backend never saw a hole -- not one segment arrived past the end of what it
already had. What it saw instead was 26 segments arriving *twice*. The device is
retransmitting data the backend has had all along, so the packet that went
missing is the acknowledgement coming back.

**The capture says the ACK was sent and lost, not delayed.** One stall, whole:

```
     213.0  down  ACK 63383     the backend has everything, 0.1 ms after it arrived
    1473.7    up  seq 60511     1261 ms of silence, then the head of the window again
    1473.9  down  ACK 63383     the same ACK, immediately
    1485.4    up  seq 63383     and the upload carries on
```

The backend acknowledged in a tenth of a millisecond and re-acknowledged the
instant it was asked. Nothing on that side was slow. The device simply never
heard it.

**Why one lost ACK costs a whole second: there is only ever 5744 bytes in
flight.** The device sends a burst, waits for the ACK, sends the next -- 2193
bursts across the run, never one byte over 5744. That is not the rig's chunking
and it is not a guess: the `sdkconfig` the prebuilt libraries are built from
says so, and every number in it is on the wire.

| In `esp32s3/sdkconfig` | | On the wire |
| --- | --- | --- |
| `CONFIG_LWIP_TCP_MSS` | 1436 | the segment size in the capture |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 5744 | four segments, the ceiling observed |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | 5760 | the window the device advertises |
| `CONFIG_LWIP_TCP_RTO_TIME` | 3000 | the timeout before an RTT has been measured |

Four segments outstanding is the whole of the problem: with one ACK in the air
there is no later cumulative ACK to cover it, so a single loss is a timeout
rather than a hiccup. **The rig's 4 KB chunks are not what produces it** --
upload 4 goes through `HTTPClient`, which writes the body in one call, and its
packets are the same four at a time against a full 5744.

Over the 64 logged uploads:

| | |
| --- | --- |
| bursts sent | 2193 |
| lost ACKs | 20, so one burst in 110 |
| first retransmit after | 999 / 1290 / 2539 ms |
| uploads that paid at least one | 17 of 64 |
| what it costs, averaged over every upload | 471 ms |

An upload is 22 bursts, so a question carries 22 chances at one-in-110 -- which
is why about one upload in four pays and the payload has nothing to do with it.
It is also exactly S7's spread: 238 ms to 6.8 s, unrelated to size.

**Power save is not it.** 9 of 32 uploads retransmitted with power save on, 8 of
32 with `WiFi.setSleep(false)`. The `Backend` numbers came out worse with
it off (median 1085 ms against 532 ms), which is the opposite direction and the
size of the noise. E6 found nothing on two round trips and E7 finds nothing on
two thousand: **measured twice, no change needed.**

**Nor is it the age of the link.** Stalls by position in the wake: 4, 3, 6 and 5
out of 16. Flat. The hypothesis this experiment was told to watch for -- that
the cost belongs to an association that has just finished -- is wrong, and four
uploads a wake is what says so.

What upload 1 *does* pay is the connect: 142 ms median against 15 ms for the
uploads behind it, on every wake. That is a link that has just come up resolving
an address it has not talked to yet. It is 130 ms on the critical path of every
question and it is not worth chasing next to the second above.

**The connects that never arrive did not arrive.** Seventy-one connections, every
SYN answered in 0.1 to 0.7 ms, no SYN ever retransmitted, nothing refused. S7 saw
four failures in about thirty questions and this run saw none in sixty-four,
which at S7's rate is a one-in-twenty-thousand coincidence -- so the two runs
differ in something, and E7 does not settle it.

It does correct the inference, though. S7 read "no file appeared in the backend's
`recordings/`" as *nothing reached the backend at all*. A file only appears when
a POST completes, so that absence proves the upload never finished and says
nothing about whether the SYN arrived. The mechanism found here is a candidate
for the same thing seen at the handshake, and the arithmetic is not comfortable.
A handshake has no RTT sample yet, so its timeout is `CONFIG_LWIP_TCP_RTO_TIME`
flat -- 3000 ms. The connect budget is 5000 ms
([`config::kBackendConnectTimeoutMs`](../../src/config.h)). So one lost SYN or
SYN-ACK is survivable with 2 s to spare and a second one is not: the retransmit
after it would be due at 9 s, four seconds past the point where the device has
already given up and shown `NO SERVER`. Unproven, and the way to prove it is a
capture running during a session that reproduces it.

**What it changes.**

- **S8 gets its number.** The round trip is 0.5 s three times out of four and
  1.5 to 5.5 s otherwise. So the working screen is not covering a predictable
  wait -- it is covering a wait that is usually shorter than a full refresh and
  occasionally four times longer. [D6](../deferred.md)'s 2.4 s is worth arguing
  about in the first case and irrelevant in the second.
- **[D4](../deferred.md) is worth more than it looked.** The cost is not a stall at
  the front: it is proportional to the number of bursts, which is proportional
  to the payload, and it lands anywhere in the body. A streaming upload that
  starts on the press puts the whole body -- stalls included -- under the hold,
  where the user is already waiting. That is the strongest argument the deferral
  has, and it scales the right way for once: a long question has more bursts and
  so more chances to stall, and a long question is the one with room to stream.

  *Taken at [S11](#firmware-streaming-validation), 2026-09-18, and
  the stalls did more than move.* 2.5 MB went up in fifteen streamed requests
  and no single write took longer than 316 ms. At the rate above -- 17 uploads
  in 64 paying a timeout of a second or more -- that much body would have paid
  about five. The likely reason is the shape of the stream rather than the
  network: a chunk goes out every 10 to 16 ms, so a lost ACK has a later one
  behind it long before a retransmission timeout comes round, which is exactly
  what the 5744-byte bursts above never had. No capture was taken to prove it.
- **[E2](e2-https-overhead.md#e2----https-overhead) can be taken now,** but only against the clean
  column. HTTP does not vary by a factor of ten by itself; it varies by a factor
  of ten one time in four, for a reason that has nothing to do with the scheme.
  The comparison has to be made on medians with the stalled uploads identified,
  not on means.
- **[S7b](e6-dhcp.md#firmware-network-validation)'s detector is safe on this
  evidence and still unproven.** Nothing in 71 connections would have been
  misread as a stale lease. But the four failures S7 saw are unaccounted for, so
  the rule still wants a second failure behind it.
- **There is a lever, and it is the one E6 found.** A larger
  `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` puts enough in flight that a lost ACK is
  covered by the next one rather than by a timeout -- which is what the same
  upload would already do on a build that had not been trimmed for RAM.

  It cannot be reached from here. `TCP_SND_BUF` is `#define`d to that symbol in
  the port's `lwipopts.h`, and the socket option that would do it at runtime is
  declared `SO_SNDBUF 0x1001 /* Unimplemented: send buffer size */` in the same
  tree. So it is a menuconfig, which means `framework = arduino, espidf` -- the
  price E6 weighed for the ARP check and declined. **That price now buys two
  things rather than one:** a third of E6's DHCP exchange and most of E7's tail.
  Neither is worth it alone; together they are worth costing out before S8
  designs a screen around the tail.

  Nothing about this belongs in the firmware today. It is a build-system change
  with its own risk, and the measurement above is what would justify it.

## Firmware upload validation

Historical S7 preceded streaming. Fifteen wakes walked the error cases and
included nine real uploads with holds from 2.3 to 12.6 seconds. Byte counts
matched recordings, including 88620 and 400428 bytes. The fixed-response
prototype had these observed round trips (upload included):

| What | Screen | Round trip |
| --- | --- | --- |
| the real endpoint | the answer, byte count matching | 238 ms - 6848 ms |
| a closed port | `NO SERVER` | 56-193 ms |
| an address nothing answers at | `NO SERVER` | 5004 ms |
| `/audio/fault/500` | `SERVER ERROR 500` | 329-4274 ms |
| `/audio/fault/empty` | `BAD RESPONSE` | 475-4284 ms |
| `/audio/fault/slow` | `TIMED OUT` | 30503-32879 ms |

The large spread triggered E7. Four failed connects across two runs were not
explained. Absence of a saved recording only proves that the POST did not
complete, not that no SYN arrived; the 71 connections in the E7 capture did
not reproduce those failures. The cache detector therefore requires two
unreachable connects, while refusal/HTTP errors preserve the lease.

## Firmware streaming validation

Historical S11, 2026-09-18, against the fixed-response prototype, after moving
display work to its own task. Ten questions and two taps, followed by an error
walk. All byte counts matched, from 23084 to 866348 bytes; neither tap opened
a request. The user checked the panel throughout.

| Hold | Request opened | Longest write | Release to the answer chirp | `WORKING` | Release to the answer on the glass |
| --- | --- | --- | --- | --- | --- |
| 3927 ms, DHCP | 3447 ms | 13 ms | 117 ms | drawn | 2199 ms |
| 3402 ms | 400 ms | 8 ms | 122 ms | drawn | 2201 ms |
| 4722 ms | 397 ms | 306 ms | 122 ms | drawn | 2201 ms |
| 10722 ms | 395 ms | 4 ms | 116 ms | drawn | 2198 ms |
| 2187 ms | 397 ms | 309 ms | 117 ms | superseded | 1830 ms |
| 2367 ms | 397 ms | 5 ms | 121 ms | superseded | 1684 ms |
| 717 ms | 397 ms | 307 ms | 154 ms | superseded | 3281 ms |
| 747 ms | 396 ms | 8 ms | 118 ms | superseded | 3279 ms |
| 912 ms | 400 ms | 6 ms | 123 ms | superseded | 3135 ms |
| 1497 ms | 397 ms | 309 ms | 121 ms | superseded | 2520 ms |


| Row | Hold | Screen | When |
| --- | --- | --- | --- |
| a closed port | 3961 ms | `NO SERVER` | 63 ms after the open, at 3.5 s -- under the hold |
| an address nothing answers at, on a lease | 3327 ms | `NO SERVER` | 18.7 s into the wake |
| `/audio/fault/500` | 2982 ms | `SERVER ERROR 500` | 138 ms after the release |
| `/audio/fault/empty` | 3207 ms | `BAD RESPONSE` | 119 ms after the release |
| `/audio/fault/slow` | 10302 ms | `TIMED OUT` | 30002 ms after the terminating chunk |
| a lease spoiled onto another subnet | 27072 ms | the answer | 122 ms after the release |
| the endpoint | 3792 ms | the answer | 115 ms after the release |

Nine of the first ten questions took 116–123 ms from release to answer chirp:
47–55 ms release confirmation, about 2 ms tail, 60 ms taken chirp, 3–4 ms
answer read and 2–3 ms other work. The outlier was 154 ms. No write across
2.5 MB exceeded 316 ms. Six of fifteen connected requests had a first write
of 303–316 ms; its cause was not established. One short release landed during
it. A later-ACK explanation for the disappearance of multi-second stalls is
plausible but was not verified by packet capture.

A deliberately spoiled lease spent two 5007 ms connects and 3211 ms DHCP under
a 27-second hold, then uploaded the entire recording from the header. A good
lease with an unreachable destination instead produced a false-positive renewal
and an error at 18.7 seconds. A refused connection aborted capture under the
hold and preserved the lease. Response timeout was 30002 ms from the terminating
chunk, confirming that recording/upload time does not consume that budget.

Compared with the preceding display-task run, LISTENING took 50–105 ms longer
while streaming; core-0 contention was suspected, not proven. Capture elapsed
time stayed within 7 ms of recorded audio. These are historical measurements,
not a claim that today's model answers in 115–154 ms.
