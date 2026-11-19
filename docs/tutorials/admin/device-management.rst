..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

###################
Device Management
###################

This tutorial covers day-to-day V80 board management using ``v80-smi``.
For the full command syntax, see :doc:`/reference/smi/commands`.

Prerequisites
=============

- SLASH platform is set up (kernel module loaded, vrtd running).
  See :doc:`platform-setup` if starting from scratch.
- ``v80-smi`` is installed and on ``PATH``.

Listing Devices
===============

Enumerate all V80 boards on the system:

.. code-block:: bash

   v80-smi list

The output shows each board's BDF address and readiness status. A board is
ready when all four checks pass:

- **PF0** (``ami``) — management interface driver loaded.
- **PF1** (``slash_qdma``) — QDMA driver loaded.
- **PF2** (``slash_ctl``) — control driver loaded.
- **VRTD** — daemon is running and the device is registered.

For detailed information including PCI IDs and driver names:

.. code-block:: bash

   v80-smi list -l

For machine-readable output:

.. code-block:: bash

   v80-smi list -j        # compact JSON
   v80-smi list -J        # pretty-printed JSON

To include sensor readings (temperature, power):

.. code-block:: bash

   v80-smi list -s

Inspecting a Vrtbin
===================

Before programming a device, inspect a vrtbin file to verify its contents:

.. code-block:: bash

   v80-smi inspect my_design.vbin

This displays the platform (Hardware/Emulation/Simulation), clock
frequency, kernel names, argument lists, and memory port connections —
all parsed from the ``system_map.xml`` inside the archive.

See :doc:`/explanation/vrtbin-format` for details on the archive structure.

Programming a Device
====================

Load a vrtbin onto a board:

.. code-block:: bash

   v80-smi program my_design.vbin -d 03:00

This extracts the PDI bitstream from the vrtbin and programs the FPGA.
The device BDF (``03:00``) can be found from ``v80-smi list``.

Each vrtbin carries a UUID. If the board already holds the design with that
UUID, nothing is written and the command returns at once. To program it
anyway:

.. code-block:: bash

   VRT_FORCE_PROGRAM=1 v80-smi program my_design.vbin -d 03:00

A design that another running application uses cannot be replaced by
programming another vrtbin: the command fails with a "busy" error until that
application exits. The same error comes back when another application is
writing a design at that moment, even the same one; run the command again.

This protection covers programming only. ``v80-smi reset``, flash programming
and hotplug operations are administrative and are not refused because an
application uses the board: they reset the board under it.

When the design is already loaded, programming changes nothing on the board:
kernels keep the state the previous application left them in, and an
application must reset its kernels before using them. If another application
is running the design, its clock is left as it is.

The board is recorded as holding the design once the transfer finished; VRTD
does not check that the card applied it. If a design that was programmed
does not respond, make VRTD forget it and program again:

.. code-block:: bash

   v80-smi forget-design -d 03:00
   v80-smi program my_design.vbin -d 03:00

After loading an image over JTAG with a tool other than
``v80-smi write-static-shell --jtag``, run ``v80-smi forget-design`` too.

Programming can also be done using VRT:

.. code-block:: cpp

   vrt::Device device("03:00", "my_design.vbin");  // programs automatically

Querying the Active Design
==========================

``v80-smi list`` shows, for each board, the UUID of the design vrtd wrote
last and how many clients use it. Compare it with the ``<UUID>`` in a
vrtbin's ``system_map.xml`` to tell whether that vrtbin is the loaded one.

To see the metadata of what was last programmed on a device:

.. code-block:: bash

   v80-smi query -d 03:00

This shows the same metadata as ``inspect`` but reads it from the device
rather than from a file on disk.

.. warning::

   ``query`` only reports what **you** (the current user) last wrote to the
   board at the given BDF — not what is physically loaded on the device right
   now. Querying the actual on-board design is not currently possible. Treat
   the output as a guide, not absolute truth.

Resetting a Device
==================

Perform a hardware reset (PCIe secondary bus reset and rescan):

.. code-block:: bash

   v80-smi reset -d 03:00

Use this when:

- A kernel is stuck and not responding to ``AP_DONE`` polling.
- You need to clear the device state before reprogramming.
- Debugging hardware issues.

After a reset, the board returns to an unprogrammed state and must be
reprogrammed before use.

Validating Memory
=================

Run memory integrity and bandwidth tests:

.. code-block:: bash

   v80-smi validate -d 03:00

This tests both HBM and DDR subsystems:

- **Integrity** — fills each memory region with a deterministic pattern
  (``i ^ seed``), reads it back, and compares.
- **Bandwidth** — measures host-to-device (H2C) and device-to-host (C2H)
  throughput.

Control the number of parallel test threads:

.. code-block:: bash

   v80-smi validate -d 03:00 -j 16    # 16 threads (default: 8, max: 64)

A passing result confirms the hardware, drivers, and memory subsystems are
functioning correctly.

Next Steps
==========

- :doc:`/reference/smi/commands` — full ``v80-smi`` command reference.
- :doc:`vrtd-configuration` — customise daemon permissions and roles.
- :doc:`/tutorials/user/getting-started` — run your first user application.
