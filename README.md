# USB 3.0 Mass-Storage Host Drivers

Linux kernel modules for studying USB mass-storage on the host: **Bulk-Only Transport (BBB/BOT)** and **USB Attached SCSI (UAS)**.

This is experimental probe-time SCSI I/O. It is **not** a replacement for the in-kernel `usb-storage` / `uas` drivers and does **not** register a block device (`/dev/sdX`).

Load **one** of the two modules at a time. They both claim mass-storage interfaces.

| Transport | Directory | Module | Device (VID:PID) | Protocol |
|-----------|-----------|--------|------------------|----------|
| BBB / BOT | repo root | `usb3_bbb.ko` | Transcend reader `8564:4000` | `0x50` — CBW / data / CSW on two bulk pipes |
| UAS | `UAS_DRIVER_PROJECT/` | `uas_driver.ko` | Cablet RTL9210 `0bda:9210` | `0x62` — Command / Status / Data IUs, SuperSpeed streams |

---

## BBB (repo root)

Host driver for a Transcend USB 3.x reader. Matches VID/PID, parses SuperSpeed companions, finds bulk IN/OUT, and runs SCSI over BBB with `usb_bulk_msg()`.

**Layout**

```
Makefile
usb3_driver.c          # probe / disconnect, usb_driver
bbb_transport.c        # CBW send, data phase, CSW check
scsi_cmds.c            # INQUIRY, TUR, REQUEST SENSE, READ CAPACITY(10), READ(10)
include/
  my_usb_dev.h
  bbb_transport.h
  scsi_cmds.h
```

**Done**

- `struct usb_driver` probe / disconnect
- SuperSpeed companion, burst, LPM (U1/U2) logging
- Bulk IN `0x81` / OUT `0x02`
- CBW generation and CSW signature / tag / status checks
- SCSI INQUIRY (`0x12`), TEST UNIT READY (`0x00`), REQUEST SENSE (`0x03`)
- SCSI READ CAPACITY(10) (`0x25`), READ(10) (`0x28`)
- MBR signature / partition peek from LBA 0
- STALL recovery with `usb_clear_halt()`

**Not done**

- WRITE(10), URB-based async I/O, mutex around commands
- Character device / sysfs / `gendisk` block layer
- Multi-LUN

**Build / load**

```bash
make
sudo rmmod usb_storage uas 2>/dev/null
sudo insmod usb3_bbb.ko
dmesg | tail -50
```

---

## UAS (`UAS_DRIVER_PROJECT/`)

Host driver for a Cablet RTL9210 (`0bda:9210`). The device enumerates with alt 0 = BBB (`0x50`). Probe switches to **alt 1 = UAS (`0x62`)**, then uses four bulk pipes and USB 3 streams.

**Layout**

```
UAS_DRIVER_PROJECT/
  Makefile
  uas_main.c                      # probe: set_interface(alt1), streams, SCSI sequence
  uas_urb_complete_functions.c    # URB complete → complete()
  uas_tur.c
  uas_inquiry.c
  uas_capacity.c
  uas_read10.c
  include/
    my_uas_dev.h
    uas_scsi.h
    uas_urb.h
```

**Done**

- Match VID/PID and MSC + SCSI + UAS (`08/06/62`)
- `usb_set_interface(..., 1)` to select UAS
- Endpoint log: CMD `0x04`, STATUS `0x83`, DATA IN `0x81`, DATA OUT `0x02`
- SuperSpeed companion: burst and MaxStreams
- `usb_alloc_streams()` (up to 32) on data-in / data-out / status; free on disconnect
- Command IU on CMD pipe; status URB with `stream_id = tag`
- SCSI TEST UNIT READY, INQUIRY, READ CAPACITY(10), READ(10) of LBA 0
- MBR bytes `510–511` printed after READ(10)
- `mutex` in `my_uas_dev` (initialized; commands still run sequentially from probe)

**Not done**

- WRITE(10), Sense IU error recovery beyond status complete
- `gendisk` / block device (`disk` field is unused)
- Full UAS task management / overlapping tagged commands from user I/O

**Build / load**

```bash
cd UAS_DRIVER_PROJECT
make
sudo rmmod usb_storage uas usb3_bbb 2>/dev/null
sudo insmod uas_driver.ko
dmesg | tail -80
```

**UAS command flow (as implemented)**

```
Host
  |-- Command IU (SCSI CDB) -------->  CMD  0x04   (bulk OUT, stream = tag)
  |<-- Data IU / payload ------------  DATA 0x81   (bulk IN,  if data-in)
  |<-- Sense / Status IU ------------  STATUS 0x83 (bulk IN,  stream = tag)
```

---

## BBB vs UAS (what this repo is for)

| | BBB | UAS |
|--|-----|-----|
| Wires | 2 bulk (IN + OUT) | 4 bulk (CMD, STATUS, DATA IN, DATA OUT) |
| Framing | CBW + optional data + CSW | Information Units (`linux/usb/uas.h`) |
| USB 3 | Works; streams not required | Streams used so tag matches status/data |
| Transfer API here | `usb_bulk_msg()` | URB submit + `completion` |

Unload the in-tree mass-storage drivers before inserting these modules, or the kernel `uas` / `usb-storage` driver will bind first.
