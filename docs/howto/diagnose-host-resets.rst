..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

##################################################
Diagnose a host that resets when the board is used
##################################################

Symptom
=======

The machine reboots with no panic, no oops and nothing in the journal. The
next boot prints a **fatal** ACPI BERT record describing a processor error:

.. code-block:: text

   x86/amd: Previous system reset reason [0x80000800]: a software sync flood event occurred
   BERT: Error records from previous boot:
   [Hardware Error]: event severity: fatal
   [Hardware Error]:  fru_text: Perr S0:T009:B05
   [Hardware Error]:    Error Structure Type: cache error
   [Hardware Error]:    Check Information: 0x000000000602001f

The record names a different CPU core on different occasions, which invites the
conclusion that the processor is defective. It is not: on one host the same
signature appeared on eight distinct cores, and a real cache defect does not
migrate.

What is actually happening
==========================

The record is written by firmware, in the only vocabulary BERT offers it. The
sequence is:

1. A driver issues MMIO at the board while its PCIe link is down -- during a
   secondary bus reset, during reconfiguration, or because the board is wedged.
2. That access is an unsupported request or a completion timeout: an
   **uncorrectable PCIe error**.
3. Error handling belongs to firmware (``GHES: APEI firmware first mode is
   enabled by APEI bit``), and its answer is a **sync flood** -- an immediate
   host reset. Linux is never handed the error, so nothing is logged.

Confirming it
=============

The crash follows a board event closely; measure the gap between the last
``RESCAN succeeded`` / ``toggle_sbr`` / ``Design write completed`` line and the
end of the journal. Clustering within a minute is the signature. The giveaway,
when it is caught, is a driver reporting nonsense read back from the board just
before the machine dies:

.. code-block:: text

   ami 0000:01:00.0: ERROR : Logging thread error - invalid PCI data read!
   eqdma_cpm5_hw_error_process: Global Err Reg(0x248) = 0xffffffff

Fixes
=====

**Do not poll a board that cannot answer.** Any thread that touches the board
on a timer must check that the device is present and that its service is not
halted before issuing MMIO. AMI's logging thread reads every 500 ms, so with a
one-second link-training window it lands inside every reset by construction.
Filtering the ``0xFF`` that comes back is too late: the damage is the read.

**Notify drivers around a reset.** Reset a live bus with ``pci_reset_bus()``,
which calls each bound driver's ``reset_prepare()`` and ``reset_done()``, not
with a bare ``pci_bridge_secondary_bus_reset()``, which resets the bus while
drivers continue to use it. Drivers must implement those callbacks for the
notification to mean anything.

**Stop firmware from escalating.** In BIOS, set ``NBIO SyncFlood Generation``
to *Disabled* (Advanced -> AMD CBS -> NBIO Common Options) and enable
``Enable AER Cap`` and EDPC/DPC. An unanswered access is then logged and
contained rather than resetting the host -- and it is the only way to see the
real PCIe error, since firmware-first destroys the evidence.

**Remove the reset windows.** Write the static shell to *both* boot partitions
(see :doc:`install-from-packages`). A board reporting ``Shell: unknown`` is
reset by the daemon on every image load; one observed boot performed 115 such
resets, each one an opportunity for the failure above.
