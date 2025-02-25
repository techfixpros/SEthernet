/*
SEthernet and SEthernet/30 Driver

Copyright (C) 2023-2024 Richard Halkyard

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <Devices.h>
#include <MacTypes.h>
#include <OSUtils.h>
#include <Slots.h>
#include <stddef.h>

#include "enc624j600.h"
#include "sethernet.h"

#if !(defined(TARGET_SE30) || defined(TARGET_SE))
#error Must define a target! (TARGET_SE or TARGET_SE30)
#endif

#if defined(TARGET_SE)
#define ENC624J600_BASE 0x800000
#endif

/* Number of protocol handlers to support */
#define numberOfPhs 16

/* Number of multicast addresses to support */
#define numberofMulticasts 8

/* ENC624J600 buffer configuration. Allocate 1536 bytes for a transmit buffer
(just enough for one frame), leaving the remainder (23040 bytes) as a receive
buffer. */
#define ENC_TX_BUF_START 0x0000
#define ENC_RX_BUF_START 0x0600

/* Protocol-handler protocol numbers are usually Ethernet II ethertypes except
for: */
enum {
  phProtocolPhaseII = 0, /* 802.2 Type 1 (ethertype < 0x600) */
  phProtocolFree = 1,    /* Invalid value used to signal a free
                            protocol-handler table entry */
};

/* Entry in our list of protocol handlers */
struct protocolHandlerEntry {
  unsigned short
      ethertype; /* Protocol number (ethertype except for those above) */
  void* handler; /* Pointer to protocol handler routine (see IM: Networking
                    chapter on ethernet protocol handlers) */
#if 0
  void* readPB;  /* Parameter block for pending ERead operation. */
#endif
};
typedef struct protocolHandlerEntry protocolHandlerEntry;

union hwAddr {
  Byte bytes[6];
  struct {
    unsigned long first4;
    unsigned short last2;
  };
};
typedef union hwAddr hwAddr;

/* Entry in our list of multicast addresses */
struct multicastEntry {
  hwAddr address;        /* Ethernet address */
  unsigned char refCount; /* Reference count */
} __attribute__((aligned (2)));
typedef struct multicastEntry multicastEntry;

/* Ethernet packet header */
struct ethernetHeader {
  hwAddr dest;               /* Destination Ethernet address */
  hwAddr source;             /* Source Ethernet address */
  unsigned short protocol;    /* Ethernet protocol/length field */
};
typedef struct ethernetHeader ethernetHeader;

/* Packet header as it appears in the ENC624J600's ring buffer */
struct ringbufEntry {
  /* Metadata from ENC624J600 */
  unsigned short nextPkt_le;  /* pointer to next packet (little-endian, relative 
                                 to chip address space) */
  enc624j600_rsv rsv;         /* Receive status vector */
  ethernetHeader pktHeader;   /* Ethernet packet header */
};
typedef struct ringbufEntry ringbufEntry;

/* Protocol handlers expect the packet header to be read into a RAM buffer (the
Receive Header Area) that includes 8 free bytes of workspace for their use */
struct receiveHeaderArea {
  ringbufEntry header;        /* Packet metadata and header */
  Byte workspace[8];          /* Protocol handler workspace */
};
typedef struct receiveHeaderArea receiveHeaderArea;

#if defined(DEBUG)
/*
Logging using MacsBug DebugStr() calls is *really* slow, and the scrollback
buffer is tiny. Instead, log interesting events in a circular buffer in memory.

Debug builds define the MacsBug macro 'dumpLog' that dumps memory from the start
of the log buffer to its current head position.
*/

#define LOG_LEN 2048

typedef enum logEvent {
  txEvent = 0x8000,
  txCompleteEvent = 0x8001,
  txCallIODoneEvent = 0x8002,
  txReturnIODoneEvent = 0x8003,
  txTaskAlreadyDeferred = 0x8004,
  txTaskAlreadyDeferredReturn = 0x8005,
  rxEvent = 0x8010,
  rxDoneEvent = 0x8011,
  readRxBufEvent = 0x8020
} logEvent;

typedef struct logEntry {
  unsigned long ticks;
  unsigned short eventType;
  unsigned short eventData;
} logEntry;

typedef struct eventLog {
  unsigned long head;
  logEntry entries[LOG_LEN];
} eventLog;
#endif

/* Global state used by the driver */
typedef struct driverGlobals {
  enc624j600 chip; /* Ethernet chip state */

  SlotIntQElement theSInt;      /* Our slot interrupt queue entry */
  AuxDCEPtr driverDCE;          /* Our device control entry */
  
  /* Flags */
  unsigned short hasGestalt : 1;  /* Gestalt Manager is available */
  unsigned short hasSlotMgr : 1;  /* Slot Manager is available */
  unsigned short vmEnabled : 1;   /* Virtual Memory is enabled */
  unsigned short macSE : 1;       /* Running on a Macintosh SE */

  protocolHandlerEntry
      protocolHandlers[numberOfPhs];             /* Protocol handler table */
  multicastEntry multicasts[numberofMulticasts]; /* Multicast address table */

  receiveHeaderArea rha;        /* Buffer for receved packet headers */

  /* The driverInfo struct is packed (dictated by the Ethernet driver API).
  Align its start point to avoid awkwardness in accessing its longword counter
  fields. */
  driverInfo info __attribute__((aligned (2)));

#if defined(DEBUG)
  eventLog log;
#endif
} driverGlobals, *driverGlobalsPtr;
