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

#include <Devices.h>
#include <Errors.h>
#include <OSUtils.h>

#include "isr.h"
#include "driver.h"
#include "enc624j600.h"
#include "enc624j600_registers.h"
#include "multicast.h"
#include "protocolhandler.h"
#include "readpacket.h"
#include "util.h"

#if defined(DEBUG)
#include <Debugging.h>
#endif

#if defined(TARGET_SE)
/* The original level-1 interrupt vector. If the interrupt fires but we don't
have a pending interrupt flag, we pass the interrupt through to it*/
void (*originalInterruptVector)();

/* Pointer to driver globals so our ISR can reference them */
driverGlobalsPtr isrGlobals;
#endif

#define likely(x)    __builtin_expect (!!(x), 1)
#define unlikely(x)  __builtin_expect (!!(x), 0)

/* IODone may trash D3 and A2-A3, which are normally assumed to be preserved
across calls. This is not documented anywhere obvious in Inside Macintosh, and
the IODone() inline function in the Universal Interfaces Devices.h does NOT save
any registers beyond the standard register spec. This routine provides a 'safe'
version that shouldn't cause any nasty register-trashing surprises. */
static inline void SafeIODone(DCtlPtr dce, OSErr result) {
  asm volatile(
    "   MOVE.L   %[dce], %%a1\n\t"
    "   MOVE.W   %[result], %%d0\n\t"
    "   MOVE.L   0x08fc, %%a0\n\t"  /* 0x08fc = IODone jump vector (JIODone) */
    "   JSR      (%%a0)\n\t"
    :
    : [dce] "g" (dce),
      [result] "g" (result)
    : "d0", "d1", "d2", "a0", "a1", /* Registers that we normally expect to be 
                                       trashed across calls */
      "d3", "a2", "a3"              /* Extra registers that IODone may change */
  );
}

/*
Wrapper to call protocol handlers from C.

Protocol handlers have an unusual register-based calling convention - it's
obvious that ethernet ISRs are intended to be all handcoded asm, but who's got
the time for that?

On protocol handler entry:
  A0: driver-specific ReadPacket argument (unused)
  A1: driver-specific ReadPacket argument (pointer to driver globals)
  A3: pointer into Receive Header Area, immediately after the header bytes
  A4: pointer to ReadPacket/ReadRest routine
  D1: number of bytes in packet (excluding header and FCS)

The handler calls ReadPacket/ReadRest with the above register definitions, but
may change any of them after calling ReadRest.

On protocol handler exit:
  A0-A5: changed
  D0-D3: changed
*/
static void callPH(enc624j600 *chip, void *phProc, Byte *payloadPtr,
                   unsigned short payloadLen) {
  asm volatile (
    "   MOVE.L    %[chip], %%a1 \n\t"
    "   MOVE.L    %[phProc], %%a2 \n\t"
    "   MOVE.L    %[payloadPtr], %%a3 \n\t"
    "   MOVE.L    %[readPacketProc], %%a4 \n\t"
    "   MOVE.W    %[payloadLen], %%d1 \n\t"
    /* Retro68's modified GCC reserves A5 as a 'fixed register' (for
    compatibility with the Mac OS 'A5 World'), which means that it will not
    generate code that touches it. Appaerntly this extends to silently ignoring
    it on asm register-usage lists! Since the protocol handler may return with
    A5 changed, we have to save and restore it ourselves. See
    https://github.com/autc04/Retro68/issues/220
    */
    "   MOVE.L    %%a5, -(%%sp) \n\t"
    "   JSR       (%%a2) \n\t"
    "   MOVE.L    (%%sp)+, %%a5 \n\t"
    : 
    : [chip] "g" (chip),
      [phProc] "g" (phProc),
      [payloadPtr] "g" (payloadPtr),
      [readPacketProc] "g" (&readPacket),
      [payloadLen] "g" (payloadLen)
    : "a0", "a1", "a2", "a3", "a4", "a5" /* ignored! */, "d0", "d1", "d2", "d3"
  );
}

/* Handle a packet from the receive FIFO */
static void handlePacket(driverGlobalsPtr theGlobals) {
  unsigned short pktLen;         /* Length of packet */
  unsigned short bytesPending;   /* Number of bytes pending in receive FIFO */
  unsigned short packetsPending; /* Number of packets pending in receive FIFO */
  unsigned char * nextPacket;    /* Pointer to next packet in buffer */
  protocolHandlerEntry *protocolSlot; /* Protocol handler */

  /* Record some FIFO stats */
  packetsPending = enc624j600_read_rx_pending_count(&theGlobals->chip);
  if (unlikely(packetsPending > theGlobals->info.rxPendingPacketsHWM)) {
    theGlobals->info.rxPendingPacketsHWM = packetsPending;
  }
  bytesPending = enc624j600_read_rx_fifo_level(&theGlobals->chip);
  if (unlikely(bytesPending > theGlobals->info.rxPendingBytesHWM)) {
    theGlobals->info.rxPendingBytesHWM = bytesPending;
  }

  /* Copy the packet header (including ENC624J600 data) into the Receive Header
  Area (RHA) - packet handlers expect this */
  readBuf(&theGlobals->chip, (unsigned char *)&theGlobals->rha.header,
          sizeof(ringbufEntry));

  /* Next-packet pointer is stored little-endian and relative to chip address
  space */
  nextPacket = enc624j600_addr_to_ptr(
      &theGlobals->chip, SWAPBYTES(theGlobals->rha.header.nextPkt_le));

  /* Packet length field in Recieve Status Vector is stored little-endian.
  Subtract 4 since this length includes the trailing checksum, which we don't
  care about */
  pktLen = SWAPBYTES(theGlobals->rha.header.rsv.pkt_len_le) - 4;

  /* Check for CRC errors. By default the ENC624J600 drops bad-CRC packets
  silently in hardware, but collect stats in case we disable that filter. */
  if (unlikely(RSV_BIT(theGlobals->rha.header.rsv, RSV_BIT_CRC_ERR))) {
    theGlobals->info.fcsErrors++;
    goto drop;
  }

  /* Check for runt frames (typically dropped in hardware, but collect stats in
  case the filter gets disabled) */
  if (unlikely(pktLen < 60)) {
    theGlobals->info.rxRunt++;
    goto drop;
  }

  /* Check for too-long frames (typically dropped in hardware, but collect stats
  in case the filter gets disabled) */
  if (unlikely(pktLen > 1514)) {
    theGlobals->info.rxTooLong++;
    goto drop;
  }

  /* Sanity-check our receive filters */
  if (RSV_BIT(theGlobals->rha.header.rsv, RSV_BIT_UNICAST)) {
    /* Destination is unicast to us */
    goto accept;
  } else if (RSV_BIT(theGlobals->rha.header.rsv, RSV_BIT_BROADCAST)) {
    /* Destination is broadcast */
    theGlobals->info.broadcastRxFrameCount++;
    goto accept;
  } else if (RSV_BIT(theGlobals->rha.header.rsv, RSV_BIT_MULTICAST) 
             && RSV_BIT(theGlobals->rha.header.rsv, RSV_BIT_HASH_MATCH)) {
      /* Destination hash matches a multicast we're listening to. It is possible
      for there to be a hash collision with another multicast address, but let's
      just ignore that */
      theGlobals->info.multicastRxFrameCount++;
  } else {
    /* Hash collision with a non-multicast address */
    theGlobals->info.rxUnwanted++;
    goto drop;
  }

accept:
  /* Find a protocol handler for this packet */
  if (likely(theGlobals->rha.header.pktHeader.protocol < 0x0600)) {
    /* An ethertype field of < 0x600 indicates an 802.2 Type 1 frame (Ethernet
    Phase II in Apple parlance). We assign this the protocol number 0. The LAP
    manager always registers itself as the handler for this protocol. */
    protocolSlot = findPH(theGlobals, phProtocolPhaseII);
  } else {
    /* Otherwise, look up a protocol handler using the ethertype field */
    protocolSlot = findPH(theGlobals, theGlobals->rha.header.pktHeader.protocol);
  }

  if (unlikely(protocolSlot == nil)) {
    /* no handler for this protocol, drop it */
    theGlobals->info.rxUnknownProto++;
    goto drop;
  }

  if (unlikely(protocolSlot->handler == nil)) {
    /* Technically, it is legal to register a protocol handler without a
    callback, indicating that it will use the ERead call to read packets. As far
    as I'm aware, this is not done by any software except for some Inside
    Macintosh code examples, and implementing ERead looks to be tricky, so for
    now it's not supported */
    DBGP("nil pointer for protocol %04x.", protocolSlot->ethertype);
    theGlobals->info.rxUnknownProto++;
    goto drop;
  }

  /* Call the protocol handler to read the rest of the packet. We've already
  read the header into the RHA, so subtract its size from the packet length. */
  debug_log(theGlobals, rxEvent, pktLen);
  callPH(&theGlobals->chip, protocolSlot->handler, theGlobals->rha.workspace,
         pktLen - sizeof(ethernetHeader));
  debug_log(theGlobals, rxDoneEvent, pktLen);
  theGlobals->info.rxFrameCount++;

drop:
  /* finished with packet, discard any remaining data by advancing the FIFO read
  pointer (and buffer tail) to the start of the next packet */
  enc624j600_update_rxptr(&theGlobals->chip, nextPacket);

  /* decrement pending-receive counter */
  enc624j600_decrement_rx_pending_count(&theGlobals->chip);
}

/* User-memory-accessing section of ISR, called through DeferUserFn when running
under Virtual Memory. Enters with IRQs already disabled, must re-enable them on
exit. */
#pragma parameter userISR(__A0)
static void userISR(driverGlobalsPtr theGlobals) {
  short irq_status = enc624j600_read_irqstate(&theGlobals->chip);

  if (likely(irq_status & IRQ_TX)) {
    /* Transmit complete; signal successful completion */

    /* Record statistics */
    unsigned short txstat =
        ENC624J600_READ_REG(theGlobals->chip.base_address, ETXSTAT);
    unsigned short collisions =
        (txstat & ETXSTAT_COLCNT_MASK) >> ETXSTAT_COLCNT_SHIFT;
    if (txstat & ETXSTAT_DEFER) {
      theGlobals->info.deferredFrames++;
    }
    if (collisions >= 1) {
      theGlobals->info.collisionFrames++;
      if (collisions == 1) {
        theGlobals->info.singleCollisionFrames++;
      } else {
        theGlobals->info.multiCollisionFrames++;
      }
    }
    theGlobals->info.txFrameCount++;

    /* Must acknowledge the transmit interrupt *before* calling IODone,
    otherwise we can accidentally acknowledge the interrupt for a transmit
    started by a completion routine */
    enc624j600_clear_irq(&theGlobals->chip, IRQ_TX);

    /* Call IODone to progress IO queue and call async completion routine */
    debug_log(theGlobals, txCallIODoneEvent, noErr);
    SafeIODone((DCtlPtr) theGlobals->driverDCE, noErr);
    debug_log(theGlobals, txReturnIODoneEvent, 0x5555);
  } else if (irq_status & IRQ_TX_ABORT) {
    /*
    Transmit aborted due to one of:
      - Collision count exceeded MACLCON_MAXRET (count in ETXSTAT_COLCNT)
      - Collision occurred after 63 bytes transmitted (ETXSTAT_LATECOL set)
      - Medium was busy, transmission deferred for longer than timeout
        (ETXSTAT_EXDEFER set)
      - Transmit aborted in software by clearing ECON1_TXRTS
    */

   /* Record statistics */
    unsigned short txstat =
        ENC624J600_READ_REG(theGlobals->chip.base_address, ETXSTAT);
    if (txstat & ETXSTAT_EXDEFER) {
      theGlobals->info.excessiveDeferrals++;
    } else if (txstat & ETXSTAT_MAXCOL) {
      theGlobals->info.excessiveCollisions++;
    } else if (txstat & ETXSTAT_LATECOL) {
      theGlobals->info.lateCollisions++;
    } else {
      theGlobals->info.internalTxErrors++;
    }

    DBGP("TX abort! ETXSTAT=%04x", txstat);

    /* Acknowledge interrupt *before* calling IODone */
    enc624j600_clear_irq(&theGlobals->chip, IRQ_TX_ABORT);

    /* Call IODone to progress IO queue and call async completion routine */
    debug_log(theGlobals, txCallIODoneEvent, excessCollsns);
    SafeIODone((DCtlPtr) theGlobals->driverDCE, excessCollsns);
    debug_log(theGlobals, txReturnIODoneEvent, 0x5555);
  }

  /* Handle any pending received packets */
  while (enc624j600_read_irqstate(&theGlobals->chip) & IRQ_PKT) {
    handlePacket(theGlobals);
    /* IRQ_PKT flag is not directly clearable - it indicates that the
    pending-receive count (decremented by handlePacket()) is nonzero */
  };

  enc624j600_enable_irq(&theGlobals->chip, IRQ_ENABLE);
}

/* Interrupt handler */
#pragma parameter __D0 driverISR(__A1)
unsigned long driverISR(driverGlobalsPtr theGlobals) {
  unsigned short irq_status;
  unsigned long irq_handled = 0;

  /* Mask all interrupts inside ISR */
  enc624j600_disable_irq(&theGlobals->chip, IRQ_ENABLE);
  irq_status = enc624j600_read_irqstate(&theGlobals->chip);

  if (unlikely(irq_status & IRQ_LINK)) {
    /* Link status has changed; update MAC duplex configuration to match
    autonegotiated PHY values */
    enc624j600_duplex_sync(&theGlobals->chip);
    enc624j600_clear_irq(&theGlobals->chip, IRQ_LINK);
    irq_handled = 1;
  }

  if (unlikely(irq_status & (IRQ_RX_ABORT | IRQ_PCNT_FULL))) {
    /* A received packet was dropped due to a full receive FIFO or
    packet-counter saturation. Unlike the DP8390 we don't need to do anything to
    recover from this state except process some pending packets. The IRQ_PKT
    interrupt handler (in the userISR() function called below) will do exactly
    that, so all we really need to do here is acknowledge the interrupt and
    increment our receive-error counter. */
    theGlobals->info.internalRxErrors++;

    DBGP("RX abort! EIR=%04x", irq_status);

    enc624j600_clear_irq(&theGlobals->chip, IRQ_RX_ABORT | IRQ_PCNT_FULL);
    irq_handled = 1;
  }

  if (likely(irq_status & (IRQ_TX | IRQ_TX_ABORT | IRQ_PKT))) {
#if defined(TARGET_SE30)
    /* Transmit and receive handlers touch user memory. When running with
    Virtual Memory enabled, this could cause a double fault (if the ISR runs
    during a page fault and the user buffer is not paged in). DeferUserFn will
    delay calling the handler until a safe time. */
    if (theGlobals->vmEnabled) {
      if (unlikely(DeferUserFn(userISR, theGlobals) != noErr)) {
        /* If we can't defer for whatever reason (usually because other ISRs
        have filled the deferral queue), re-enable interrupts and return
        "interrupt not handled" status. When the ISR fires again (immediately,
        because the ENC624J600 is still asserting an IRQ), we can try again. */
        enc624j600_enable_irq(&theGlobals->chip, IRQ_ENABLE);
        return 0;
      } else {
        /* Successfully deferred our call to userISR. Return 'interrupt handled'
        status. Since userISR may not actually run until after we return, we
        leave the ENC624J600's interrupts disabled, and let userISR re-enable
        them when it completes. */
        return 1;
      }
    } else {
      /* No VM, just call the handler directly. */
      userISR(theGlobals);
      irq_handled = 1;
    }
#elif defined(TARGET_SE)
      /* SE doesn't support VM, just call the handler directly. */
      userISR(theGlobals);
      irq_handled = 1;
#endif
  }

  if (irq_handled == 0) {
    DBGP("Spurious interrupt! EIR=%04x", irq_status);
  }

  enc624j600_enable_irq(&theGlobals->chip, IRQ_ENABLE);
  return irq_handled;
}
