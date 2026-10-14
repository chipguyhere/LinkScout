# LinkScout

**A two-tool field kit for AV and network installers**, on a Waveshare ESP32-P4 wall panel.

<table>
  <tr>
    <td align="center" width="33%"><img src="docs/main-menu.jpg" alt="LinkScout boot menu with two large buttons, Cresnet Monitor and Switch Port Finder"></td>
    <td align="center" width="33%"><img src="docs/cresnet-monitor.jpg" alt="Cresnet Monitor showing a 16 by 16 grid of Cresnet IDs, most green, a few magenta, the rest grey"></td>
    <td align="center" width="33%"><img src="docs/port-finder.jpg" alt="Switch Port Finder showing link up at 100 Mbps, the switch name, port 5, its system description and the DHCP address"></td>
  </tr>
  <tr>
    <td align="center"><b>Pick a tool at boot</b></td>
    <td align="center"><b>Cresnet Monitor</b><br>every ID: polled, answering, or gone quiet</td>
    <td align="center"><b>Switch Port Finder</b><br>which switch and port this jack lands on</td>
  </tr>
</table>

When the panel boots, it shows two large buttons. The tool you tap runs until the next power
cycle:

| Tool | Answers | Connects via |
| --- | --- | --- |
| **Cresnet Monitor** | Which Cresnet IDs does the control processor poll, and which of them answer? | The panel's built-in RS485 terminal |
| **Switch Port Finder** | Which switch, port and VLAN is this wall jack patched to? | The panel's RJ45 Ethernet port |

To switch tools, press the panel's reset button or power-cycle it. Tap the dim **Info** label
in the lower-right corner of the menu to see the build date, the author and the open-source
notices.

**Always start this tool at power-on.** Tick this box on the menu, then pick a tool. From
then on, the panel skips the menu at power-on and goes straight into that tool, which suits
a unit left in place. To get the menu back, **press the reset button**. The menu then shows
the box still ticked; untick it to go back to choosing at every boot. Installing a new
version clears the choice.

## What you need

**Waveshare ESP32-P4-86-Panel-ETH-2RO**: an 86 × 86 mm wall-box panel with a 720×720 touch
display. Its bottom board carries the RJ45 Ethernet jack, an isolated RS485 port, two relays
and a 12–24 V power input. Nothing else is needed: no external transceiver, no adapters.

> **Check the SKU: you need the `-ETH-2RO` model.** Waveshare also sells this panel
> without the bottom board, so with **neither Ethernet nor RS485**. That model's name
> leaves out `ETH`, and it's typically listed as the version with a **camera** port.
> **Neither tool works on it**: the Switch Port Finder needs the Ethernet jack, and the
> Cresnet Monitor needs the RS485 port. The firmware will install and show its menu, but
> there's nothing for either tool to listen to.

## Installing: one click

> [!WARNING]
> **Clicking the link below starts flashing almost straight away, without asking "are you
> sure?".** As soon as you pick a port after **Connect**, the installer erases the entire
> flash of the ESP32 device on that port, including whatever firmware it runs now and
> anything it has stored, and writes LinkScout. Make sure the port you pick is the panel you
> mean to install on.

In **Chrome or Edge on a desktop computer** (these browsers can talk to USB serial ports;
Firefox and Safari can't):

1. Plug the panel into the computer with a USB **data** cable (some cables only charge),
   using the panel's **bottom** USB-C port, the one **furthest from the red light**. The
   other USB-C port can't be used for installing.
2. Open **[the LinkScout installer](https://chipguyhere.github.io/esp-launchpad/minimal-launchpad/?flashConfigURL=https://raw.githubusercontent.com/chipguyhere/LinkScout/main/launchpad.toml)**
   (see the warning above).
3. Click **Connect** and pick the panel's port, listed as **USB Single Serial**. The installer
   erases the panel and writes the newest firmware straight from this repository; there's
   nothing to download first. Erasing all 32 MB of flash takes a while, so give it a few
   minutes. You don't need to hold any buttons.
4. Press the panel's **reset** button, or unplug it and plug it back in.

If the port doesn't appear or the installer can't connect, see
[other ways to install](docs/install-other-methods.md).

## Cresnet Monitor

A **passive** Cresnet bus monitor. It listens to the bus and never transmits. It shows every
possible Cresnet ID as a 16×16 hex grid, coloured by what the control processor is doing with
it:

| Colour | Meaning |
| --- | --- |
| Green | Responding to polls |
| Yellow | Was responding; the latest poll went unanswered |
| Red | Was responding earlier, silent for the past minute |
| Magenta | Polled, but never seen to respond since the panel started |
| Dark grey | Not currently polled |

Tap the grid to show this legend on the panel. If no bus traffic is seen for 5 seconds, a
**Master Offline** plate appears with a wiring reminder. IDs `00`, `01`, `02` (master side)
and `FF` (broadcast) are never peripherals and get no cell.

**Wiring.** Connect all four Cresnet conductors to the panel's 4-pin terminal, and the panel
runs from Cresnet power on a single cable:

| Cresnet | Panel terminal |
| --- | --- |
| **Y** | pin 1, **A** |
| **Z** | pin 2, **B** |
| **G** | pin 3, **GND** |
| **24** | pin 4, **+12–24 V** |

The panel's draw comes out of that Cresnet power budget like any other device on the bus.
If you'd rather not load it, connect only Y and Z and power the panel from USB-C. In that
case leave pin 3 unconnected.

**Termination.** The panel has a 120 Ω terminating resistor across A–B, so connecting it adds
a termination wherever you clip in. In practice Cresnet keeps working with it there. If you'd
rather the panel didn't load the bus, the resistor can be removed:
[removing the terminating resistor](docs/termination-resistor.md).

## Switch Port Finder

Listens on the Ethernet port for the two discovery protocols switches use to announce
themselves:

- **LLDP** (IEEE 802.1AB)
- **CDP** (Cisco Discovery Protocol)

It shows what the switch reports about the port you're plugged into: switch name, port name,
port description, VLAN, management address, platform and software version. Every value is
labelled with the protocol field it came from, because switch vendors produce some cryptic
strings. The panel also requests a DHCP lease and shows the address it gets, the panel's MAC
address, and a live link band (speed and duplex).

Behaviour worth knowing:

- **Switches advertise every 30–60 seconds**, so expect a short wait after plugging in.
- **Stale data turns grey.** When the link drops, everything learned from it stays on screen
  in grey. It returns to full colour only when it's learned again on the new link.
- **Receive-only.** The panel doesn't send LLDP or CDP, so it won't appear in the switch's
  neighbour table.
- **Unmanaged switches** usually send neither protocol, so the panel stays on "Listening for
  LLDP / CDP". DHCP and link state still work.

## Building from source

The full source is in this repository. See [BUILDING.md](BUILDING.md) for building it, making
releases and how the firmware works inside.

## Credits

Written by **Michael Caldwell-Waller** — [github.com/chipguyhere](https://github.com/chipguyhere).

## License

The sketch is MIT licensed; see [LICENSE](LICENSE). The firmware also contains third-party
components under their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and
[`LICENSES/`](LICENSES). Ship both with any copy of the binary.

Crestron and Cresnet are trademarks of Crestron Electronics, Inc. Cisco and CDP are
trademarks of Cisco Systems, Inc. This project is not affiliated with or endorsed by either
company. Cresnet support comes from passively observing bus traffic.
