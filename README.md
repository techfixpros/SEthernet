# SEthernet and SEthernet/30

Modern, low-cost 10/100 Ethernet for the Macintosh SE and SE/30.

## Introduction

It's an unfortunate reality of retrocomputing that, as time goes on, original
hardware only ever becomes more scarce, and usually more expensive.

While PDS ethernet cards are certainly not _scarce_ yet, the collector appeal of
Compact Macs means that cards are in high demand, with consequent high prices.
As of this writing (October 2023), the going eBay rate for them seems to be of
the order of $US 150-200, which is a bit steep for those of us who remember
finding them in electronics-recycler dollar bins.

While other projects have aimed to clone these original cards, such an approach
comes with a downside - the ethernet controller chips used in them have been out
of production for many years, and the controllers and (even more so) their
supporting components, are difficult to find.

While cloned cards trade off parts sourcing difficutly (and cost) for the
significant advantage of being able to use existing drivers, this project takes
the opposite approach: design an all-new card using a modern, readily-available
ethernet controller, and write a new driver for it. How hard can it be?
[^howhard]

## Hardware Overview

Both the SEthernet and SEthernet/30 use the Microchip
[ENC624J600](https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/39935c.pdf)
all-in-one embedded ethernet controller. It's cheap, readily available, has a
straightforward software interface without any glaring misfeatures (I'm looking
at you, RTL8019AS), and amongst its multitude of configurable host-bus modes, it
has one that matches the 68k processor bus nicely.

Since the Macintosh SE lacks any real infrastructure for expansion beyond
"here's a connector with the CPU bus on it," the SEthernet board is a very dumb
device. It simply maps the ENC624J600 into a vacant chunk of memory at `0x80
0000`-`0x80 ffff` and connects it to an interrupt line. Hardware detection,
interrupt vector interception, and device configuration are all left to the
driver to implement.

The Macintosh SE/30 is a more civilised machine, and the SEthernet/30 provides a
[declaration ROM](rom/se30) and multiple addressing options (configurable by
jumper) to take full advantage of the Slot Manager and coexist with multiple PDS
cards. Potentially, the driver could even be built into the declaration ROM,
allowing for a truly plug-and-play solution. To facilitate driver updates, the
declaration ROM is a flash chip, with logic to allow for in-system programming.

## Current project status

Very early days yet! Pardon my dust as I get this repo organised, more thorough
writeups are hopefully forthcoming.

Rev0 boards have been ordered for the SE and SE/30, and the driver and glue
logic are theoretically complete but untested and almost certainly broken in
many fun and exciting ways.

The plan is, once the boards arrive, to bring up the SE board first, given that
its glue logic is simpler. Once that's functional, I'll move on to the SE/30.

A subsequent revision will likely eliminate the awful through-hole PLCC sockets
for directly-soldered surface-mount chips. This should make for much better
routing and potentially free up board real estate for a PDS pass through slot in
the SE/30 card.

While I would like to produce a small production run of these once the design
has stabilised, that is likely a ways off at this point.

## Project files

Schematics: [SEthernet](boards/se/se.pdf) -
[SEthernet/30](boards/se30/se30.pdf) - [Breakout Board](boards/breakout/breakout.pdf)

Glue Logic: [SEthernet](pld/se) - [SEthernet/30](pld/se30)

[SEthernet30 Declaration ROM](rom/se30)

[Driver](software/driver)

## Required tools

Schematics, board layout and BOM: [KiCad 7](https://www.kicad.org/)

Programmable Logic: [WinCUPL](https://www.microchip.com/en-us/products/fpgas-and-plds/spld-cplds/pld-design-resources)
(see [`pld/README.md`](pld/README.md) for notes on WinCUPL)

Declaration ROM, driver and software:
[Retro68](https://github.com/autc04/Retro68), [CMake](https://cmake.org/) and
[Python 3](https://www.python.org)

## Errata

### Revision 0

#### ENC624J600 is erroneously configured into SPI mode

The combined `SPISEL` `/INT` pin on the ENC624J600 is pulled up instead of down.
This causes the ENC624J600 to be configured into SPI mode rather than parallel
mode, which is not much use to us.

##### Symptoms

Card does not function.

##### Workaround

Break the track between the `SPISEL` pullup resistor (R5 on SEthernet, R2 on
SEthernet/30) and 3.3V. Connect the resistor to ground instead.

##### Resolution

This issue has been resolved in the latest schematics for Revision 1.

#### Read accesses to the SEthernet/30 board do not meet timing specifications.

##### Symptoms

Card operation is erratic.

##### Workaround

Insert wait states on read accesses

- Carefully cut the traces connecting U4 pin 4 to ground on both sides of the
  board.

- Program a GAL16V8/ATF16V8 with the equations in
  [`pld/se30/se30-bodge.pld`](pld/se30/se30-bodge.pld) and 'dead bug' it to a
  convenient place on the circuit board, connected as follows:

Pin #  | Destination
-------|------------
1      | PDS connector, pin A38 (`CPUCLK`)
2      | U3, pin 12 (`!ROM_CS`)
3      | U3, pin 13 (`EN_CS`)
10, 11 | Ground
12     | U4, pin 4 (`WAIT`)
20     | +5V

Connect a 0.1uF capacitor across pins 10 and 20.

##### Resolution

Revision 1 glue logic will be implemented on single CPLD with configurable
wait-state generation built in.

## Useful reference material

- [ENC624J600 datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/39935c.pdf)

- [Designing Cards and Drivers for the Macintosh Family (3rd
  Edition)](https://www.vintageapple.org/inside_o/pdf/Designing_Cards_and_Drivers_for_the_Macintosh_Family_3rd_Edition_1992.pdf)

- [Inside Macintosh (1994): Networking](https://www.vintageapple.org/inside_r/pdf/Networking_1994.pdf)

- [Inside Macintosh (1994): Devices](https://www.vintageapple.org/inside_r/pdf/Devices_1994.pdf)

- [Glenn Anderson's ethernet driver page](https://www.mactcp.net/ethernet.html) -
  contains C source for a DP8390-based card.

- [Mac Driver Museum: Network
  Drivers](https://vintageapple.org/macdrivers/network.shtml) - good material
  for reverse-engineering. In particular, the Dayna ethernet drivers contain
  debug symbols, which are quite handy for our purposes.

- [Rob Braun's declaration ROM page](http://www.synack.net/~bbraun/declrom.html)
  has useful information about declaration ROMs and how they work.

[^howhard]: [Arrested Development narrator voice]: It was, in fact, quite hard
