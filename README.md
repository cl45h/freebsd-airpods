# AirPods on FreeBSD

Connect Apple AirPods (or any A2DP Bluetooth headphones) to FreeBSD using `blued`, `virtual_oss`, and a Realtek USB Bluetooth adapter.

Tested on **FreeBSD 15.0-RELEASE** with AirPods Pro (1st gen). Should work with any A2DP-capable Bluetooth audio device.

## Why?

FreeBSD's built-in Bluetooth stack (`hcsecd` + `ng_hci`) doesn't support SSP (Simple Secure Pairing), which modern headphones like AirPods require. This setup uses [`blued`](https://github.com/niccokunzmann/blued) as a replacement Bluetooth daemon that handles SSP properly, combined with `virtual_oss` compiled with Bluetooth/A2DP support to bridge audio.

## Hardware

- **Realtek RTL8761BU USB Bluetooth adapter** (USB ID `0bda:8771`)
  - The Intel onboard adapter (`ubt0`) may work with some devices, but AirPods specifically required the Realtek
  - Other Realtek BT adapters supported by `rtlbtfw(8)` should also work
- The onboard Intel adapter stays as `ubt0`, the Realtek becomes `ubt1` after firmware load

## Architecture

```
App (Waterfox, mpv, etc.)
  → PulseAudio (sink: airpods_pro)
    → /dev/dsp (CUSE device, created by virtual_oss)
      → virtual_oss (SBC encoder + A2DP/AVDTP)
        → L2CAP socket (Bluetooth)
          → Realtek USB adapter (ubt1)
            → AirPods
```

## Installation

### 1. Install packages

```sh
pkg install blued rtlbt-firmware pulseaudio
```

### 2. Compile virtual_oss from source

The `virtual_oss` package doesn't include Bluetooth support. You need to compile from source:

```sh
git clone https://github.com/hselasky/virtual_oss.git
cd virtual_oss
make HAVE_BLUETOOTH=YES HAVE_BLUETOOTH_SPEAKER=YES HAVE_COMMAND=YES HAVE_SNDSTAT=YES
sudo make install
```

This installs to `/usr/local/sbin/virtual_oss`. The package version at `/usr/sbin/virtual_oss` does NOT have Bluetooth — make sure you use the full path or that `/usr/local/sbin` comes first in your `$PATH`.

### 3. Create firmware symlink

`rtlbtfw` looks for firmware in `/usr/share/firmware/rtlbt/` but the package installs to `/usr/local/share/rtlbt-firmware/`:

```sh
sudo mkdir -p /usr/share/firmware
sudo ln -s /usr/local/share/rtlbt-firmware /usr/share/firmware/rtlbt
```

### 4. Configure Bluetooth

Add your device to `/etc/bluetooth/hosts`:

```
cc:22:fe:67:67:cf airpods
```

Configure blued to use the Realtek adapter. Edit `/usr/local/etc/blued.conf`:

```
hci_node = "ubt1hci";
```

### 5. Configure rc.conf

```sh
# Enable blued
sysrc blued_enable="YES"

# Disable the default virtual_oss service (we manage it ourselves)
sysrc virtual_oss_enable="NO"

# Add cuse to kernel modules loaded at boot
# (append to your existing kld_list)
sysrc kld_list+=" cuse"
```

### 6. Install devd rule for auto firmware loading

```sh
sudo cp rtlbt.conf /usr/local/etc/devd/
sudo service devd restart
```

### 7. Install the connection script

```sh
sudo cp airpods /usr/local/bin/airpods
sudo chmod +x /usr/local/bin/airpods
```

Edit the script and change these variables to match your device:

```sh
AIRPODS_MAC="cc:22:fe:67:67:cf"   # Your device's BT MAC address
AIRPODS_NAME="airpods"             # Hostname from /etc/bluetooth/hosts
OWNER="cl45h"                      # Your username (for PulseAudio control)
PA_RESTORE_SINK="oss_output.dsp6"  # Your default speaker sink
```

## Usage

### First time pairing

Put your AirPods (or headphones) in pairing mode, then:

```sh
sudo airpods connect
```

The script will scan, pair, connect, start the audio bridge, and route PulseAudio.

### Reconnecting

After a reboot or disconnect, just run:

```sh
sudo airpods connect
```

### Disconnecting

```sh
sudo airpods disconnect
```

This stops the audio bridge and restores PulseAudio to your default speakers.

### Status

```sh
sudo airpods status
```

## Troubleshooting

### "Could not create CUSE DSP device"

A previous `virtual_oss` process left a stale handle on `/dev/cuse`. Fix:

```sh
# Find and kill the zombie
sudo fstat /dev/cuse
sudo kill -9 <PID>

# If that doesn't work, reload the module
sudo kldunload cuse && sudo kldload cuse

# Then retry
sudo airpods connect
```

### "BT connection failed"

The link key is out of sync. Reset your AirPods (hold the case button for 15 seconds until the light flashes amber then white) and pair again:

```sh
sudo bluecontrol unpair cc:22:fe:67:67:cf
sudo airpods connect
```

### Audio plays but no sound in browser

Make sure PulseAudio is routing to the right sink:

```sh
pactl set-default-sink airpods_pro
```

If the sink doesn't exist, the script may need to re-add it:

```sh
pactl load-module module-oss device=/dev/dsp sink_name=airpods_pro sink_properties=device.description="AirPods_Pro"
pactl set-default-sink airpods_pro
```

### Realtek adapter not detected (no ubt1)

Load the firmware manually:

```sh
sudo rtlbtfw -d ugen0.3   # adjust device name
```

### Importing a link key from Linux

If your headphones were previously paired on Linux (e.g., dual-boot), you can avoid re-pairing by checking the link key at:

```
/var/lib/bluetooth/<adapter_mac>/<device_mac>/info
```

However, the key is tied to the specific Bluetooth adapter. It will only work on FreeBSD if you use the same physical adapter.

## How it works

1. **`rtlbtfw`** loads firmware to the Realtek USB adapter, making it available as `ubt1` in the netgraph Bluetooth stack
2. **`blued`** manages Bluetooth pairing and connections via SSP (Simple Secure Pairing), which the default `hcsecd` doesn't support
3. **`virtual_oss`** creates a virtual audio device (`/dev/dsp`) backed by a Bluetooth A2DP connection — it handles SDP discovery, AVDTP signaling, and SBC encoding
4. **PulseAudio** routes application audio to the virtual device, making it transparent to browsers and other apps

## airpods-ctl — AirPods control tool

A native C tool to control AirPods features from FreeBSD via the AACP (Apple Accessory Communication Protocol) over L2CAP. Based on the protocol reverse-engineered by [LibrePods](https://github.com/kavishdevar/librepods).

### Build

```sh
cd /path/to/freebsd-airpods
make -f Makefile.ctl
sudo make -f Makefile.ctl install
```

### Usage

The AirPods must be connected via `bluecontrol` first.

```sh
# ANC control
airpods-ctl anc on              # Noise cancellation (isolate external sound)
airpods-ctl anc off             # No noise control
airpods-ctl anc transparency    # Hear surroundings
airpods-ctl anc adaptive        # Adaptive mode

# Conversational Awareness
airpods-ctl ca on
airpods-ctl ca off

# Status
airpods-ctl battery             # Battery levels (left, right, case)
airpods-ctl ear                 # Ear detection state
airpods-ctl info                # Device name, model, firmware
airpods-ctl listen              # Listen for all notifications

# Use a specific device address
airpods-ctl -d cc:22:fe:67:67:cf battery
```

### How it works

`airpods-ctl` opens an L2CAP socket to PSM `0x1001` (the AACP control channel, separate from the A2DP audio channel at PSM `0x19`), performs a protocol handshake, and sends/receives control packets. All commands are simple hex byte sequences — no encryption required.

## Credits

- [blued](https://github.com/niccokunzmann/blued) by JRG Systems — Bluetooth daemon with SSP support for FreeBSD
- [virtual_oss](https://github.com/hselasky/virtual_oss) by Hans Petter Selasky — virtual audio device with Bluetooth A2DP backend
- [JRG Systems blog post](https://jrgsystems.com/posts/2022-09-06-blued/) — original guide that made this possible
- [LibrePods](https://github.com/kavishdevar/librepods) — reverse-engineered AirPods protocol (AACP), used as reference for `airpods-ctl`

## License

The code in this repository is released under the BSD 2-Clause License. See individual upstream projects for their licenses.
