#include "driver.h"

#include <AppleTalk.h>
#include <Devices.h>
#include <ENET.h>
#include <Errors.h>
#include <Gestalt.h>
#include <MacTypes.h>
#include <Resources.h>
#include <Slots.h>
#include <string.h>
#include <Retro68Runtime.h>

#include "enc624j600.h"
#include "isr.h"
#include "multicast.h"
#include "protocolhandler.h"
#include "registertools.h"
#include "util.h"

#if defined(DEBUG)
#include <Debugging.h>
#include <stdio.h>
char strbuf[255];
#endif

/*
EWrite (a.k.a.) Control called with csCode=ENetWrite

Initiate transmission of an ethernet frame. This function is asynchronous and
returns as soon as the frame has been copied into the transmit buffer and
transmission has been started. Completion is signaled through a
transmit-complete or transmit-aborted interrupt.

From my rudimentary understanding of IO on the Classic Mac OS, the Device
Manager handles the queueing of writes for us and won't issue another ENetWrite
until the last one has signaled completion.

The frame data is given as a Write Data Structure (WDS) - a list of
address-length pairs like an iovec, we need to read from each one in sequence,
the end of the WDS is signaled by an entry with a zero length. The ethernet
header is already prepared for us, we just have to write our hardware address
into the source field.
*/
OSStatus doEWrite(driverGlobalsPtr theGlobals, EParamBlkPtr pb) {
  WDSElement *wds; /* a WDS is a list of address-length pairs like an iovec */
  short entryLen;  /* length of current WDS entry */
  unsigned long totalLength; /* total length of frame */
  Byte *source, *dest;

  /* Scan through WDS list entries to compute total length */
  wds = (WDSElement *)pb->u.EParms1.ePointer;
  totalLength = 0;
  do {
    totalLength += wds->entryLength;
    wds++;
  } while (wds->entryLength);

  /* Block transmission of oversized frames. (Add 4 bytes to calcullated length
  to account for FCS field generated by ethernet controller) */
  if (totalLength + 4 > 1518) {
  #if defined(DEBUG)
    strbuf[0] = sprintf(strbuf+1, "TX: bogus length %lu bytes!", totalLength);
    DebugStr((unsigned char *)strbuf);
  #endif
    return eLenErr;
  }

  /* Restore WDS pointer to start of list */
  wds = (WDSElement *)pb->u.EParms1.ePointer;
  source = (Byte *)wds->entryPtr;
  dest = theGlobals->chip.base_address + ENC_TX_BUF_START;
  entryLen = wds->entryLength;

  /* Copy data from WDS into transmit buffer */
  do {
    enc624j600_memcpy(dest, source, entryLen);
    dest += entryLen;
    wds++;
    entryLen = wds->entryLength;
    source = (Byte *)wds->entryPtr;
  } while (entryLen > 0);

  /* Go back and copy our address into the source field */
  dest = theGlobals->chip.base_address + ENC_TX_BUF_START + 6;

  enc624j600_memcpy(dest, theGlobals->info.ethernetAddress, 6);

  if (theGlobals->chip.link_state == LINK_DOWN) {
    /* don't bother trying to send packets on a down link */
    return 0; /* TODO: find an appropriate error code */
  }

  /* Send it! */
  enc624j600_transmit(&theGlobals->chip, theGlobals->chip.base_address,
                      totalLength);

  /* Return >0 to indicate operation in progress */
  return 1;
}

/*
Entrypoint for Open call.

Called whenever software opens the ethernet driver, regardless of whether it is
already open.

If driver is not open, allocate storage, initialize data structures, and set the
chip up.

If driver is already open, do nothing.
*/
#pragma parameter __D0 driverOpen(__A0, __A1)
OSErr driverOpen(__attribute__((unused)) IOParamPtr pb, AuxDCEPtr dce) {
  driverGlobalsPtr theGlobals;
  Handle eadrResourceHandle;
  OSStatus error;

  error = noErr;

  if (dce->dCtlStorage == nil) {
    /* 
    Unlike classic Mac OS toolchains, Retro68 does NOT generate PC-relative
    code, and if you use -mpcrel to force it to, it'll appear to work for simple
    programs but start to come unraveled as things get more complicated. For
    simplicity's sake, it's much easier to just live with relocation rather than
    fighting it.

    For applications, the Retro68 runtime automatically relocates us at startup,
    but for non-application code such as a driver, we have to call the relocator
    ourselves before we can access global and static variables, or call
    functions.
    */
    RETRO68_RELOCATE();

    theGlobals = (driverGlobalsPtr)NewPtrSysClear(sizeof(driverGlobals));
    if (!theGlobals) {
      error = MemError();
    } else {
      /* dCtlStorage is technically a Handle, but since its use is entirely
      user-defined we can just treat it as a pointer */
      dce->dCtlStorage = (Handle)theGlobals;

#if defined(TARGET_SE30)
      long gestaltResult;

      /* Set up chip base address the clever way. We could manually compute the
      address from the slot number in dCtlSlot, but that means that we have to
      hard-code the location of the chip in the card's address space.
      dCtlDevBase points at the slot base address (i.e. Fs00 0000), with an
      optional offset defined by sResources. This means that cards with
      different address decoding will work with this driver so long as they have
      appropriate sResources (MinorBaseOS and/or MajorBaseOS) in ROM. */
      theGlobals->chip.base_address = (void *)dce->dCtlDevBase;

      /* Check if running under virtual memory, make ourselves VM-safe if so.
      See 'Driver Considerations for Virtual Memory' in Technote NW-13 */
      if ((Gestalt(gestaltVMAttr, &gestaltResult) == noErr) &&
          (gestaltResult & (1 << gestaltVMPresent))) {
        theGlobals->usingVM = true;
        /* Ask the memory manager to not page our data out */
        HoldMemory(theGlobals, sizeof(driverGlobals));
        dce->dCtlFlags |= dVMImmuneMask; /* Tell the OS that we're VM-safe */
      }
#elif defined(TARGET_SE)
      /* SE: base address is hardcoded. Try writing and reading back a value to
      probe for hardware. No need to worry about virtual memory here! */
      theGlobals->chip.base_address = (void *) ENC624J600_BASE;
      volatile unsigned long * test = (unsigned long *) theGlobals->chip.base_address;
      *test = 0x123455aa;
      if (*test != 0x123455aa) {
        DisposePtr((Ptr)theGlobals);
        dce->dCtlStorage = nil;
        return openErr;
      }
#endif

      /* Save our device control entry - we need this to signal completion of IO
      at interrupt time */
      theGlobals->driverDCE = dce;

      if (dce->dCtlFlags & dRAMBasedMask) {
        /* If loaded via a Handle, detach our driver resource. This means that
        the Resource Manager can no longer 'see' it, preventing it from being
        changed, released, etc. Unfortunately this also means that Macsbug's
        heap analyzer can no longer identify it either :( */
        DetachResource((Handle)dce->dCtlDriver);
      }

      /* Initialize protocol-handler table */
      InitPHTable(theGlobals);

      /* Reset the chip */
      enc624j600_reset(&theGlobals->chip);

      /* Wait for the chip to come back after the reset. According to the
      datasheet, we must delay 25us for bus interface and MAC registers to come
      up, plus an additional 256us for the PHY. I'm not aware of any easy way to
      delay with that kind of granularity, so just busy-wait for 1 tick */
      waitTicks(1);

      /* Initialize the ethernet controller. */
      enc624j600_init(&theGlobals->chip, ENC_RX_BUF_START);

      /* Figure out our ethernet address. First we look for an 'eadr' resource
      with an ID corresponding to our slot. If one exists, we save it to our
      globals, and write it into the chip. Otherwise, we read the chip's address
      (which the reset above restored to its factory-assigned value) into our
      globals. */
      eadrResourceHandle = GetResource(EAddrRType, dce->dCtlSlot);
      if (eadrResourceHandle) {
        copyEthAddrs(theGlobals->info.ethernetAddress, (Byte *)*eadrResourceHandle);
        enc624j600_write_hwaddr(&theGlobals->chip, (Byte *)*eadrResourceHandle);
        ReleaseResource(eadrResourceHandle);
      } else {
        enc624j600_read_hwaddr(&theGlobals->chip,
                               theGlobals->info.ethernetAddress);
      }

      /* Set up read pointers to the start of the receive FIFO */
      theGlobals->nextPkt =
          enc624j600_addr_to_ptr(&theGlobals->chip, ENC_RX_BUF_START);

#if defined(TARGET_SE30)
      /* Install our interrupt handler using the Slot Manager */
      theGlobals->theSInt.sqType = sIQType;
      theGlobals->theSInt.sqPrio = 250;
      theGlobals->theSInt.sqAddr = driverISR;
      theGlobals->theSInt.sqParm = (long)theGlobals;
      SIntInstall(&theGlobals->theSInt, dce->dCtlSlot);
#elif defined(TARGET_SE)
      isrGlobals = theGlobals;
      asm (
        /* No Slot Manager on the SE, we hook the Level 1 Interrupt vector. Very
        Commodore 64-style. Level 1 is normally used by the VIA and SCSI
        controller, so we have to coexist with them. */
        /* Mask interrupts while we change out interrupt vectors */
        "MOVE.W  %%sr, -(%%sp) \n\t"
        "ORI.W   %[srMaskInterrupts], %%sr  \n\t"
        /* Save the original vector*/
        "MOVE.L  0x64, %[originalInterruptVector]  \n\t"
        /* Install our own */
        "MOVE.L  %[driverISR], 0x64  \n\t"
        /* Restore interrupts */
        "MOVE.W  (%%sp)+, %%sr"
        : [originalInterruptVector] "=m" (originalInterruptVector)
        : [driverISR] "r" (driverISR),
          [srMaskInterrupts] "i" (0x700)
      );
#endif

#if defined(DEBUG)
      /* Let's go! */
      strbuf[0] = sprintf(strbuf + 1, "Driver opened. Globals at %08x", 
                          (unsigned int) theGlobals);
      DebugStr((unsigned char *) strbuf);
#endif
      enc624j600_start(&theGlobals->chip);
      enc624j600_enable_irq(&theGlobals->chip,
                            IRQ_ENABLE | IRQ_LINK | IRQ_PKT | IRQ_RX_ABORT |
                                IRQ_PCNT_FULL | IRQ_TX | IRQ_TX_ABORT);
    }
  } else {
    /* Driver was already open, nothing to do */
    error = noErr;
  }
  return error;
}

/*
Entrypoint for Close call

Ethernet drivers don't generally get closed, as drivers don't (can't?) implement
reference counting and software has no way of knowing if other software is using
it. Still, drivers all seem to implement some kind of token shutdown procedure.
*/
#pragma parameter __D0 driverClose(__A0, __A1)
OSErr driverClose(__attribute__((unused)) IOParamPtr pb, AuxDCEPtr dce) {
  driverGlobalsPtr theGlobals = (driverGlobalsPtr)dce->dCtlStorage;

  /* Reset the chip; this is just a 'big hammer' to stop transmitting, disable
  receive, disable interrupts etc. */
  enc624j600_reset(&theGlobals->chip);

#if defined(TARGET_SE30)
  /* Uninstall our slot interrupt handler */
  SIntRemove(&theGlobals->theSInt, dce->dCtlSlot);
#elif defined(TARGET_SE)
  asm volatile (
    /* Mask interrupts while we change out interrupt vectors */
    "MOVE.W  %%sr, -(%%sp) \n\t"
    "ORI.W   %[srMaskInterrupts], %%sr  \n\t"
    /* Restore the original interrupt vector */
    "MOVE.L  %[originalInterruptVector], 0x64  \n\t"
    /* Restore interrupts */
    "MOVE.W  (%%sp)+, %%sr"
    :
    : [originalInterruptVector] "m" (originalInterruptVector),
      [srMaskInterrupts] "i" (0x700)
  );
#endif

  if (theGlobals->usingVM) {
    /* Unpin if running with virtual memory */
    UnholdMemory(theGlobals, sizeof(driverGlobals));
  }
  DisposePtr((Ptr)theGlobals);
  dce->dCtlStorage = nil;

  return noErr;
}

/*
Control entrypoint

This is where the magic happens. Dispatch to various operations based on the
csCode in the parameter block.

Note that control operations can be asynchronous! The wrapper code in header.s
handles this for us, all we need to do is return a value <=0 when returning
synchronously (0 for success, <0 for error) or >0 for async operations that will
be completed by a later IODone call.
*/
#pragma parameter __D0 driverControl(__A0, __A1)
OSErr driverControl(EParamBlkPtr pb, DCtlPtr dce) {
  driverGlobalsPtr theGlobals = (driverGlobalsPtr)dce->dCtlStorage;
  switch (pb->csCode) {
    case ENetDelMulti: /* Delete address from multicast table */
      return doEDelMulti(theGlobals, pb);
    case ENetAddMulti: /* Add address to multicast table */
      return doEAddMulti(theGlobals, pb);
    case ENetAttachPH: /* Attach receive handler for ethertype */
      return doEAttachPH(theGlobals, pb);
    case ENetDetachPH: /* Detach receive handler for ethertype */
      return doEDetachPH(theGlobals, pb);
    case ENetRead:       /* Read packets directly without a handler routine */
      return controlErr; /* TODO: support this */
    case ENetRdCancel:   /* Cancel a pending ENetRead */
      return controlErr; /* TODO: support this */
    case ENetWrite:      /* Send packet */
      return doEWrite(theGlobals, pb);
    case ENetGetInfo: /* Read hardware address and statistics */
      /* We use an extended version of the driver info struct with some extra
      fields tacked onto the end. Note that we do not have counters for all the
      standard fields. */
      if (pb->u.EParms1.eBuffSize > (short) sizeof(theGlobals->info)) {
        pb->u.EParms1.eBuffSize = sizeof(theGlobals->info);
      }

      BlockMoveData(&theGlobals->info, pb->u.EParms1.ePointer,
                    pb->u.EParms1.eBuffSize);
      return noErr;

    case ENetSetGeneral: /* Enter 'general mode' */
      /* ENEtSetGeneral tells the driver to prepare to transmit general Ethernet
      packets rather than only AppleTalk packets. Drivers can use this to
      rearrange TX/RX buffer boundaries for the longer maximum frame length
      (1536 vs. 768 bytes). We have enough buffer to always operate in general
      mode, so this is a no-op. */
      return noErr;

#if 0
    /* I've seen these csCodes in the wild but my headers don't define them, and
    some drivers (e.g. MACE) that do include them don't actually do anything
    with them. Probably safe to just not implement I guess? */
    case EGetDot3Entry:
    case ESetDot3Entry:
    case EGetDot3Statistics:
    case EGetDot3CollStats:
    case LapGetLinkStatus:
      return noErr;
    case ENetEnablePromiscuous:
    case ENetDisablePromiscuous:
      reeturn controlErr;
#endif

    /* Custom csCodes for debugging this driver. */
    case ENCReadReg: /* Read ENC624J600 register */
      return doENCReadReg(theGlobals, (CntrlParamPtr)pb);
    case ENCWriteReg: /* Write ENC624J600 register */
      return doENCWriteReg(theGlobals, (CntrlParamPtr)pb);
    case ENCReadPhy: /* Read ENC624J600 PHY register */
      return doENCReadPhy(theGlobals, (CntrlParamPtr)pb);
    case ENCWritePhy: /* Write ENC624J600 PHY register */
      return doENCWritePhy(theGlobals, (CntrlParamPtr)pb);
    case ENCEnableLoopback: /* Enable PHY internal loopback */
      return doENCEnableLoopback(theGlobals);
    case ENCDisableLoopback: /* Disable PHY internal loopback */
      return doENCDisableLoopback(theGlobals);

    default:
#if defined(DEBUG)
      strbuf[0] = sprintf(strbuf + 1,
                        "Unhandled csCode %d", pb->csCode);
      DebugStr((unsigned char *) strbuf);
#endif
      return controlErr;
  }
}
