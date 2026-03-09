# voljack

`voljack` is a minimal and simple JACK client that provides a **single shared volume control for any number of JACK audio channels**.

Each input channel is passed through a shared gain stage and routed directly to its corresponding output:

NOTE: This is a vibe coded work in progress, I know little more than how to write a for loop in C, this is probably trash as far as implementation goes. Improvements welcome.

```
inX → gain → outX
```

All channels share the same gain value.

The goal is **simplicity, reliability, and negligible CPU overhead**.

When running, `voljack` appears as a normal JACK client (e.g. in `qjackctl`) with ports:

```
in0 ... inN
out0 ... outN
```

---

# Features

- extremely lightweight JACK client
- any number of channels
- one shared volume control
- dB-based gain control
- default startup volume: **-inf (mute)**
- configurable minimum and maximum volume
- CLI volume control commands
- automatic first-run configuration
- minimal dependencies

---

# Dependencies

- C compiler (`gcc` or `clang`)
- `make`
- JACK development library (`libjack`)
- `pkg-config`
- `libm` (math library, normally included with libc)

---

# Build

Clone the repository and run:

```bash
make
```

This produces the binary:

```
voljack
```

---

# Install

System-wide install:

```bash
sudo install -m755 voljack /usr/local/bin/
```

Optional manual uninstall:

```bash
sudo rm /usr/local/bin/voljack
```

---

# First Run

If `voljack` is started with **no config and no `-c` parameter**, it will prompt for channel count:

```
No config found.
Enter channel count:
```

The configuration file will then be automatically created at:

```
~/.config/voljack/config
```

Example config:

```
IO=6
MIN_DB=-inf
MAX_DB=10
START_DB=-inf
STEP_DB=1.0
SOCKET=/tmp/voljack.sock
```

---

# Running voljack

Start the JACK client:

```bash
voljack
```

Or specify channel count manually:

```bash
voljack -c 6
```

Once running, `voljack` will appear in JACK patchbay tools.

---

# Volume Control

Volume is controlled via CLI commands that communicate with the running instance.

### Get current volume

```bash
voljack get
```

Example output:

```
-12.00 dB
```

---

### Set volume

```bash
voljack set -18
voljack set 3
voljack set -inf
```

---

### Increase volume

```bash
voljack inc
voljack inc 0.5
```

Uses `STEP_DB` from config when no value is given.

---

### Decrease volume

```bash
voljack dec
voljack dec 2
```

---

# Volume Range

Default range:

```
MIN_DB = -inf
MAX_DB = +10 dB
```

These can be changed in the config file.

---

# Signal Path

For each channel:

```
input → gain → output
```

Gain is shared across all channels.

Amplitude conversion:

```
gain = 10^(dB / 20)
```

Special case:

```
dB = -inf → gain = 0
```

---

# Example JACK Ports

For `-c 4`:

```
voljack:in0
voljack:in1
voljack:in2
voljack:in3

voljack:out0
voljack:out1
voljack:out2
voljack:out3
```

