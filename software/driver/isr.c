#include "isr.h"

#include "driver.h"
#include "enc624j600.h"
#include "enc624j600_registers.h"
#include "multicast.h"
#include "protocolhandler.h"

#if defined(DEBUG)
#include <Debugging.h>
#include <stdio.h>
extern char strbuf[255];
#endif

#if defined(TARGET_SE)
/* The original level-1 interrupt vector. If the interrupt fires but we don't
have a pending interrupt flag, we pass the interrupt through to it*/
void (*originalInterruptVector)();

/* Pointer to driver globals so our ISR can reference them */
driverGlobalsPtr isrGlobals;
#endif

/* ReadPacket callback function that we pass to protocol handlers. We don't (and
shouldn't) call it ourselves */
extern void ReadPacket();

/*
Wrapper to call protocol handlers from C.

Protocol handlers have an unusual register-based calling convention - it's
obvious that ethernet ISRs are intended to be all handcoded asm, but who's got
the time for that?

On protocol handler entry:
  A0: driver-specific ReadPacket argument (pointer to driver globals)
  A1: driver-specific ReadPacket argument (pointer to protocol handler)
  A3: pointer into Receive Header Area, immediately after the header bytes
  A4: pointer to ReadPacket/ReadRest routine
  D1: number of bytes in packet (excluding header and FCS)

The handler calls ReadPacket/ReadRest with the above register definitions, but
may return with them destroyed.

On protocol handler exit:
  A0-A5: destroyed
  D0-D3: destroyed
*/
static void callPH(enc624j600 *chip, void *phProc, Byte *payloadPtr,
                   unsigned short payloadLen) {
  asm volatile (
    "   MOVE.L    %[chip], %%a0 \n\t"
    "   MOVE.L    %[phProc], %%a1 \n\t"
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
    "   JSR       (%%a1) \n\t"
    "   MOVE.L    (%%sp)+, %%a5 \n\t"
    : 
    : [chip] "g" (chip),
      [phProc] "g" (phProc),
      [payloadPtr] "g" (payloadPtr),
      [readPacketProc] "g" (&ReadPacket),
      [payloadLen] "g" (payloadLen)
    : "a0", "a1", "a2", "a3", "a4", "a5" /* ignored! */, "d0", "d1", "d2", "d3"
  );
}

/* Handle a packet from the receive FIFO */
static void handlePacket(driverGlobalsPtr theGlobals) {
  unsigned short pktLen;         /* Length of packet */
  unsigned short bytesPending;   /* Number of bytes pending in receive FIFO */
  unsigned short packetsPending; /* Number of packets pending in receive FIFO */
  protocolHandlerEntry *protocolSlot; /* Protocol handler */

  /* Record some FIFO stats */
  packetsPending = enc624j600_read_rx_pending_count(&theGlobals->chip);
  if (packetsPending > theGlobals->info.rxPendingPacketsHWM) {
    theGlobals->info.rxPendingPacketsHWM = packetsPending;
  }
  bytesPending = enc624j600_read_rx_fifo_level(&theGlobals->chip);
  if (bytesPending > theGlobals->info.rxPendingBytesHWM) {
    theGlobals->info.rxPendingBytesHWM = bytesPending;
  }

  /* Copy the packet header (including ENC624J600 data) into memory */
  enc624j600_read_rxbuf(&theGlobals->chip, (unsigned char *) &theGlobals->rha, 
    sizeof(theGlobals->rha) - sizeof(theGlobals->rha.workspace));

  /* Packet length field in Recieve Status Vector is stored little-endian */
  pktLen = SWAPBYTES(theGlobals->rha.rsv.pkt_len_le);

  /* Check for CRC errors. By default the ENC624J600 drops bad-CRC packets
  silently in hardware, but collect stats in case we disable that filter. */
  if (RSV_BIT(theGlobals->rha.rsv, RSV_BIT_CRC_ERR)) {
    theGlobals->info.fcsErrors++;
    goto drop;
  }

  /* Check for runt frames (typically dropped in hardware, but collect stats in
  case the filter gets disabled) */
  if (pktLen < 64) {
    theGlobals->info.rxRunt++;
    goto drop;
  }

  /* Check for too-long frames (typically dropped in hardware, but collect stats
  in case the filter gets disabled) */
  if (pktLen > 1518) {
    theGlobals->info.rxTooLong++;
    goto drop;
  }

  /* Sanity-check our receive filters */
  if (RSV_BIT(theGlobals->rha.rsv, RSV_BIT_UNICAST)) {
    /* Destination is broadcast or unicast to us */
    goto accept;
  } else if (RSV_BIT(theGlobals->rha.rsv, RSV_BIT_BROADCAST)) {
    theGlobals->info.broadcastRxFrameCount++;
    goto accept;
  } else if (RSV_BIT(theGlobals->rha.rsv, RSV_BIT_MULTICAST) 
             && RSV_BIT(theGlobals->rha.rsv, RSV_BIT_HASH_MATCH)) {
    /* Destination hash matches a multicast we're listening to */
    if (findMulticastEntry(theGlobals, theGlobals->rha.dest)) {
      /* Actual destination address matches a multicast we're listening to */
      theGlobals->info.multicastRxFrameCount++;
      goto accept;
    } else {
      /* Multicast hash collision */
      theGlobals->info.rxUnwanted++;
      goto drop;
    }
  } else {
    /* Hash collision with a non-multicast address */
    theGlobals->info.rxUnwanted++;
    goto drop;
  }

accept:
  /* An ethertype field of < 0x600 indicates an 802.2 Type 1 frame (Ethernet
  Phase II in Apple parlance). We assign this the protocol number 0. The LAP
  manager always registers itself as the handler for this protocol. */
  if (theGlobals->rha.protocol < 0x0600) {
    protocolSlot = findPH(theGlobals, phProtocolPhaseII);
  } else {
    protocolSlot = findPH(theGlobals, theGlobals->rha.protocol);
  }
  /* Search the protocol-handler table for a handler for this ethertype */
  if (protocolSlot == nil) {
    /* no handler for this protocol, drop it */
    goto drop;
  }

  if (protocolSlot->handler == nil) {
#if defined(DEBUG)
    strbuf[0] = sprintf(strbuf+1, "nil pointer for protocol %04x.", 
                        protocolSlot->ethertype);
    DebugStr((unsigned char *)strbuf);
#endif
    goto drop;
  }

  /*
  Call the protocol handler to read the rest of the packet.

  pktLen-18 is length of packet minus header (6+6+2 bytes) and trailing checksum
  (4 bytes)
  */
  callPH(&theGlobals->chip, protocolSlot->handler, theGlobals->rha.workspace,
         pktLen-18);
  theGlobals->info.rxFrameCount++;

drop:
  /* finished with packet, discard any remaining data by advancing read pointer
  and rx buffer tail to the start of the next packet  */
  enc624j600_update_rxptr(&theGlobals->chip, enc624j600_addr_to_ptr(
      &theGlobals->chip, SWAPBYTES(theGlobals->rha.nextPkt_le)));

  /* decrement pending-receive counter */
  enc624j600_decrement_rx_pending_count(&theGlobals->chip);
}

/* User-memory-accessing section of ISR, called through DeferUserFn on VM
systems. Enters with IRQs already disabled, must re-enable them on exit. */
static void userISR(driverGlobalsPtr theGlobals) {
  short irq_status = enc624j600_read_irqstate(&theGlobals->chip);

  if (irq_status & IRQ_TX) {
    /* Transmit complete; signal successful completion */
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
    IODone((DCtlPtr)theGlobals->driverDCE, noErr);
    enc624j600_clear_irq(&theGlobals->chip, IRQ_TX);
  }

  if (irq_status & IRQ_TX_ABORT) {
    /*
    Transmit aborted due to one of:
      - Collision count exceeded MACLCON_MAXRET (count in ETXSTAT_COLCNT)
      - Collision occurred after 63 bytes transmitted (ETXSTAT_LATECOL set)
      - Medium was busy, transmission deferred for longer than timeout
        (ETXSTAT_EXDEFER set)
      - Transmit aborted in software by clearing ECON1_TXRTS

    Not sure how best to express these so for simplicity let's just call them
    all timeouts.
    */
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

#if defined(DEBUG)
    strbuf[0] = sprintf(strbuf+1, "TX abort! ETXSTAT=%04x", txstat);
    DebugStr((unsigned char *)strbuf);
#endif

    IODone((DCtlPtr)theGlobals->driverDCE, excessCollsns);
    enc624j600_clear_irq(&theGlobals->chip, IRQ_TX_ABORT);
  }

  /* We have pending packets. Handle them. */
  while (enc624j600_read_irqstate(&theGlobals->chip) & IRQ_PKT) {
    handlePacket(theGlobals);
    /* IRQ_PKT flag is not directly clearable - it indicates that the
       pending-receive count (decremented by handlePacket()) is nonzero */
  };

  enc624j600_enable_irq(&theGlobals->chip, IRQ_ENABLE);
}

/* Interrupt handler, wrapped by driverISR below. */
__attribute__((used)) static unsigned long _driverISR(
    driverGlobalsPtr theGlobals) {
  unsigned short irq_status;
  unsigned long irq_handled = 0;

  /* Mask all interrupts inside ISR */
  enc624j600_disable_irq(&theGlobals->chip, IRQ_ENABLE);
  irq_status = enc624j600_read_irqstate(&theGlobals->chip);

  if (irq_status & IRQ_LINK) {
    /* Link status has changed; update MAC duplex configuration to match
    autonegotiated PHY values */
    enc624j600_duplex_sync(&theGlobals->chip);
    enc624j600_clear_irq(&theGlobals->chip, IRQ_LINK);
    irq_handled = 1;
  }

  if (irq_status & (IRQ_RX_ABORT | IRQ_PCNT_FULL)) {
    /* Packet dropped due to full receive FIFO or packet-counter saturation.
    Unlike the DP8390 we don't need to do anything to recover from this state
    except process some pending packets (below) */
    theGlobals->info.internalRxErrors++;

#if defined(DEBUG)
    strbuf[0] = sprintf(strbuf+1, "RX abort! EIR=%04x", irq_status);
    DebugStr((unsigned char *)strbuf);
#endif

    enc624j600_clear_irq(&theGlobals->chip, IRQ_RX_ABORT | IRQ_PCNT_FULL);
    irq_handled = 1;
  }

  if (irq_status & (IRQ_TX | IRQ_TX_ABORT | IRQ_PKT)) {
    /* Transmit and receive handlers touch user memory. On Virtual Memory
    systems, this could cause a double fault if the ISR runs during a page fault
    and the user buffer is not paged in. DeferUserFn will delay calling the
    handler until a safe time. */
    if (theGlobals->usingVM) {
      if (DeferUserFn(userISR, theGlobals) != noErr) {
        /* If we can't defer for whatever reason, re-enable interrupts and
        return "interrupt not handled" status */

        enc624j600_enable_irq(&theGlobals->chip, IRQ_ENABLE);
        return 0;
      }
    } else {
      /* No VM, just call the handler directly. */
      userISR(theGlobals);
    }

    /* Note that in this case we return immediately without re-enabling IRQs;
    userISR may run AFTER we return if it gets deferred, so we leave re-enabling
    IRQs to it */
    return 1;
  }

#if defined(DEBUG)
  if (irq_handled == 0) {
    strbuf[0] = sprintf(strbuf+1, "Spurious interrupt! EIR=%04x", irq_status);
    DebugStr((unsigned char *)strbuf);
  }
#endif

  enc624j600_enable_irq(&theGlobals->chip, IRQ_ENABLE);
  return irq_handled;
}

/*
Our interrupt service routine - actually just a wrapper that calls the 'real'
_driverISR() routine above.

On the SE/30, we register our ISR with the Slot Manager, so all we need to do is
wrap it in some MOVEMs to preserve the right registers.

On the SE, there is no system-provided mechanism to register expansion-hardware
ISRs, so we hijack the vector for the interrupt level we use (Level 1). Level 1
is also used by the VIA, so our ISR must query the card's interrupt status, and
decide whether to service it with _driverISR() or jump to the original Level 1
vector to service a VIA interrupt.
*/
void driverISR(void) {
#if defined(TARGET_SE30)
  /*
  SE/30: register-preservation wrapper for slot interrupt

  On entry:
    A1: contents of Slot Interrupt Queue entry sqParm field

  On exit:
    A1: destroyed
    A0,A2-6: preserved
    D0: 1 if IRQ was handled, 0 otherwise
    D1-D7: preserved

  C functions always save A2-A6 and D3-D7, so we just need to save A0, D1, D2.
  */
  asm("MOVEM.L %A0/%D1-%D2, -(%SP)\n\t"
      "MOVE.L %A1, -(%SP)\n\t"
      "JSR _driverISR\n\t"
      "ADDQ #4, %SP\n\t"
      "MOVEM.L (%sp)+, %A0/%D1-%D2");
#elif defined(TARGET_SE)
  asm volatile (
    /* Read the ENC624J600 EIR register to see if we have an interrupt from it */
    "   MOVE.W  %%d0, -(%%sp) \n\t"
    "   MOVE.W  (%[estat_reg]), %%d0 \n\t"
    "   ANDI.W  %[estat_int_mask], %%d0  \n\t"
    "   BEQ     not_us_%= \n\t"

    /* We have an interrupt waiting: call our ISR */
    "   MOVEM.L %%a0-%%a1/%%d1-%%d2,  -(%%sp) \n\t"
    "   LEA     isrGlobals, %%a1 \n\t"
    "   MOVE.L  %%a1, -(%%sp) \n\t"
    "   JSR     _driverISR  \n\t"
    "   ADDQ    #4, %%sp \n\t"
    "   MOVEM.L (%%sp)+, %%a0-%%a1/%%d1-%%d2\n\t"
    "   MOVE.W  (%%sp)+, %%d0  \n\t"
    "   RTE\n"

    /* The interrupt wasn't us: chain through to the original vector */
    "not_us_%=:\n\t"
    "   MOVE.W  (%%sp)+, %%d0  \n\t"
    "   MOVE.L  originalInterruptVector, -(%%sp)  \n\t"
    "   RTS"  /* push-and-RTS is apparently the quickest way to jump through a
              vector in memory. Once again, shades of the C64 here */
    :
    : [estat_int_mask] "n" (ESTAT_INT),
      [estat_reg] "a" (ENC624J600_REG(ENC624J600_BASE, ESTAT))
      /* UGLY HACK: mark non-saved registers as in-use so that any
      automatically-allocated registers will be saved and restored */
    : "a0", "a1", "d0", "d1", "d2"
  );
#endif
}
