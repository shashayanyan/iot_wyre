# LoRaChat — SX127x Firmware Shell Reference

A LoRa mesh chat firmware for RIOT-OS running on SX127x-based hardware. Nodes communicate over LoRa radio using a structured frame format, with support for direct messages, group salons, mesh relaying, telemetry, and distress signals. \
The main file can be found in iot_wyre/tests/driver_sx127x/main.c \
The other main was used as a backup. (main_youssef)
---

## Message Frame Format

All LoRaChat messages follow this structure:

```
<emitter><type><target>:<msg_id>,<ttl>:<payload>
```

| Field | Description |
|---|---|
| `emitter` | Sender node ID (e.g. `6767`) |
| `type` | `@` direct/broadcast · `#` salon · `*` broadcast |
| `target` | Peer node ID, salon name, or empty for broadcast |
| `msg_id` | 16-bit sequence number |
| `ttl` | Time-to-live (hop count, **OPTIONAL!**) |
| `payload` | Message content |

**Example:** `6767@NODE2:42,7:Hello there`

---

## Setup & Initialization

### `init`
Initialize the SX1272 radio hardware and start receiver, telemetry, and relay threads.

```
> init
```

Must be called before any radio operation. Starts three background threads: `recv_thread`, `telemetry_thread`, and `relay_thread`.

---

### `setup <bandwidth> <spreading_factor> <code_rate>`
Configure LoRa modulation parameters.

```
> setup 125 7 5
```

| Parameter | Values |
|---|---|
| bandwidth | `125`, `250`, `500` (kHz) |
| spreading_factor | `7` to `12` |
| code_rate | `5` to `8` |

---

### `channel <get\|set> [frequency_hz]`
Get or set the radio frequency channel.

```
> channel get
> channel set 868100000
```

---

### `syncword <get\|set> [value]`
Get or set the LoRa sync word (hex byte).

```
> syncword get
> syncword set 12
```

---

### `listen`
Switch the radio to continuous receive mode.

```
> listen
```

The radio auto-returns to listen mode after each transmission, but this command forces it manually.

---

### `reset`
Hardware-reset the SX127x chip.

```
> reset
```

---

## Messaging

### `chat <target> <message...> [ttl=N]`
Send a LoRaChat message. Target prefix determines delivery mode.

| Target prefix | Meaning |
|---|---|
| `@<nodeID>` | Direct message to a specific node |
| `#<salon>` | Message to a salon (must be joined first) |
| `*` | Broadcast to all nodes |

```
> chat @NODE2 Hello there
> chat #frblabla Salut tout le monde
> chat * Qui est la?
> chat @NODE2 Hello ttl=3
```

Default TTL is `7`. Optionally override with `ttl=N`.

#### Special payloads

**RDV — Frequency rendezvous**
```
> chat * RDV 868100000 SF7BW125
```
Instructs other nodes to switch to the specified frequency and LoRa config. Frequency must be in the 860–870 MHz range.

**SOS — Distress signal**
```
> chat * sos 3.86292 11.50003
```
Broadcasts a distress signal with GPS coordinates. Receiving nodes display a Google Maps link.

**LPP — Cayenne Low Power Payload telemetry**
```
> chat * lpp 036701100567AABB   # Send pre-formatted hex LPP data
> chat * lpp                    # Request SAUL sensor reading
```

---

### `apply_rdv`
Apply the last received RDV (rendezvous) radio parameters.

```
> apply_rdv
```

After receiving an RDV message, the new frequency/SF/BW are stored as pending. This command commits them to the radio. Can only be applied once per received RDV.

---

### `send <payload>`
Send a raw string payload without LoRaChat framing.

```
> send HelloWorld
```

---

### `sendhex <hex_payload>`
Send a raw hexadecimal binary payload.

```
> sendhex 48656C6C6F
```

---

## Salons (Group Channels)

### `join <salon_name>`
Join a salon to receive and send messages to that group. The `#` prefix is optional.

```
> join frblabla
> join #emergency
```

Up to 5 salons can be joined simultaneously.

---

### `leave <salon_name>`
Leave a salon.

```
> leave frblabla
```

---

### `salons`
List all currently joined salons.

```
> salons
```

---

## Network & Nodes

### `nodes` / `users`
List all known nodes seen on the network, with their last message ID and time since last contact.

```
> nodes
```

---

## Mesh Relay

Received messages with TTL > 0 are automatically queued for relay. The relay thread processes the queue with SNR-based backoff delays — nodes with a weaker signal to the sender relay sooner (higher priority), nodes with a strong signal wait longer to avoid redundant relays.

### `snr_threshold <get|set <dB>>`
Get or set the SNR threshold for relay filtering. Messages received with SNR **above** the threshold are not relayed (the sender has a strong signal and likely doesn't need help).

```
> snr_threshold get
> snr_threshold set 5
```

Default threshold is `0 dB` (relay everything).

---

### `relayq`
Show the current relay queue status: pending messages, their delays, TTL, and SNR values.

```
> relayq
```

---

## Telemetry

### `autotelm <seconds>`
Enable or disable automatic periodic telemetry transmission using SAUL sensors (temperature, humidity). Reads onboard sensors and sends LPP Cayenne formatted packets.

```
> autotelm 10    # Send every 10 seconds
> autotelm 0     # Disable
```

---

## History

### `history`
Display the last received messages (ring buffer, up to 10 entries), including sender, target, TTL, SNR, and relay status.

```
> history
```

Relay status values: `RELAYED`, `NO_RELAY`, `DROP`.

---

## Persistence (EEPROM)

The node table is automatically saved to EEPROM whenever it is updated, and restored on boot. Checksum validation protects against corrupted data.

### `forget`
Wipe the persisted node table from EEPROM (factory reset of known nodes).

```
> forget
```

---

## Radio Diagnostics

### `random`
Read a hardware random number from the SX127x entropy source.

```
> random
```

---

### `register <get|set> [reg] [value]`
Read or write SX127x hardware registers directly.

```
> register get all         # Dump full register map
> register get allinline   # Dump registers on one line
> register get 0x01        # Read register 0x01
> register set 0x01 0xFF   # Write 0xFF to register 0x01
```

---

### `crc set <1|0>`
Enable or disable CRC checking on received packets.

```
> crc set 1
```

---

### `implicit set <1|0>`
Enable or disable implicit (fixed) header mode.

```
> implicit set 0
```

---

### `payload set <length>`
Set the expected payload length (used in implicit header mode).

```
> payload set 32
```

---

### `rx_timeout set <value>`
Set the RX symbol timeout.

```
> rx_timeout set 100
```

---

## Quick Start Example

```
> init
> setup 125 7 5
> channel set 868100000
> listen
> join #mygroup
> chat #mygroup Hello from node 6767
> autotelm 30
> nodes
> history
```

---

## Node ID

This node's ID is hardcoded as `MY_NODE_ID = "6767"`. Change the `#define MY_NODE_ID` in the source to customize it.