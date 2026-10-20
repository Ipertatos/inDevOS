# User-mode drivers for inDevOS

Status: design (step 1). Only `acpidump` (section 9) is implemented.
First target: the I2C-HID touchpad on Georgios's AMD laptop.

The rule throughout: **use the Linux interface where one exists**, so that
existing tools (`evtest`, `libevdev`, SDL's evdev backend, `python-uinput`,
UIO test programs) work unchanged on inDevOS, and so that Linux driver code
and documentation map directly onto ours.

---

## 1. What inDevOS has today (survey, Oct 2026)

| Area | State | Notes for this design |
|---|---|---|
| ACPI tables | `acpi.c`: RSDP from Limine, RSDT/XSDT walk, MADT, FADT, HPET, MCFG | Tables only. |
| AML | **No interpreter** (no uACPI, no ACPICA). `parse_s5_from_dsdt()` byte-scans the DSDT for `_S5_` | Cannot evaluate `_CRS`, `_STA`, `_DSM`, `_PS0`. |
| IOAPIC | `apic.c`: `ioapic_redirect_irq(lapic, vector, irq, enable)`, honours MADT ISOs | No `_PRT`; PCI INTx is routed as *edge, active-high* using the PCI Interrupt Line register as the GSI. Level/active-low only comes from ISOs. |
| Vectors | `idt.c`: stubs only for vectors 0-35, 43-48, 253-255; `isr_handler()` is an if-chain on fixed numbers | No allocator, no registration API, no sharing. **Vector 45 is used by both the PS/2 mouse and pcnet.** |
| MSI/MSI-X | `pci.c`: `pci_msi_enable()`, `pci_msix_enable()/configure_vector()` | Usable once there is a vector allocator. |
| PCI | `pci_devices[]` (256), IDs, class, BARs via `pci_device_read()` | |
| Device mmap | `sys_mmap_device(addr,len,prot,fixed,phys,pte_extra)` (added for `/dev/fb0`): maps physical pages with `VMM_DEVICE` so munmap/exit/fork never free or COW them; `pte_extra` picks UC/WC | Exactly what UIO mmap needs. |
| Linux fds | Per-process `fds[16]` (`MAX_OPEN_FILES_SYSCALL`), `fd_type` switch in read/write/ioctl/poll/close/fstat (`FD_TYPE_DEV_FB`=9, eventfd=8, ...) | Each new device type adds another branch in ~6 places; see 3.6. |
| poll | `sys_poll()` re-evaluates every fd, then `thread_sleep(1)` (1 ms) and loops | No wait queues; 1 ms latency is fine for input. |
| eventfd, futex, pipes, sockets | Present | |
| Input: keyboard | PS/2 IRQ1 -> `keyboard.c`; USB HID boot keyboard converts to set-1 scancodes -> `keyboard_inject_scancode()` | |
| Input: mouse | PS/2 IRQ12 / polled -> `mouse_process_packet()` -> globals `mouse_x/y/buttons`, `luade_mouse_event()`, SDL accumulators. USB boot mouse -> `mouse_feed_usb_relative(dx,dy,buttons)` (same sinks, **no wheel**) | The desktop polls `mouse_get_position()/mouse_get_buttons()`; LuaDE gets events. |
| evdev / uinput | **None** (`/dev/input` does not exist) | |
| Loadable code | `kmod.c` (ELF relocatable `.ko`, `insmod/rmmod/lsmod`), `driver_manager.c` (register/enable/disable built-in drivers) | The `.sys`-style route already exists; this design is about the user-mode route. |
| Permissions | `process_t.uid`, `su`, users file | Root check is easy (`uid == 0`). |

---

## 2. Architecture

```
                 user mode                                  kernel
 +-------------------------------+        +--------------------------------------+
 | drvd.exe (supervisor)         |        | ACPI tables (+ uACPI later)          |
 |  reads /etc/drivers.conf      |------->| /dev/uioctl  claim / release         |
 |  starts, watches, restarts    | claim  | UIO core: /dev/uioN, /sys/class/uio  |
 +-------------------------------+        |   mmap(MMIO) read(irq) write(unmask) |
 | i2chid.exe (touchpad)         |<------>| IRQ core: vector alloc, IOAPIC, MSI, |
 |  libdwi2c  libi2chid  libhid  |  uio   |   AMD GPIO pin demux                 |
 |  gestures                     |        |                                      |
 |      | write(input_event)      |        | input core  <- PS/2 kbd/mouse        |
 |      v                        |        |             <- USB HID kbd/mouse     |
 |  /dev/uinput -----------------+------->|             <- uinput devices        |
 +-------------------------------+        |   |-> /dev/input/eventN (evdev)      |
 | evtest, SDL, games            |<-------|   |-> pointer sink: mouse_x/y,       |
 +-------------------------------+ evdev  |        LuaDE, desktop, SDL           |
                                          |   '-> kbd sink: keyboard_inject_*    |
                                          +--------------------------------------+
```

Three kernel pieces (input core + evdev + uinput, IRQ core, UIO), one control
device, one supervisor process, and then drivers as ordinary static musl
`.exe` files.

---

## 3. Device access: UIO

Modelled on Linux UIO (`Documentation/driver-api/uio-howto.rst`), with the
same file semantics so existing UIO code ports directly.

### 3.1 `/dev/uioN`

* `open()` - root only (uid 0), and only after the device was claimed (3.4).
  One opener at a time (`-EBUSY` otherwise), like a VFIO group.
* `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, N * PAGE_SIZE)` maps
  memory region *N* (Linux convention: the offset selects the map). Backed by
  `sys_mmap_device()` with UC (`PCD|PWT`) PTEs; WC on request via an inDevOS
  flag for prefetchable BARs. The kernel refuses anything outside the claimed
  regions.
* `read(fd, &u32, 4)` blocks until the next interrupt and returns the
  cumulative interrupt count (Linux semantics). `O_NONBLOCK` -> `-EAGAIN`.
* `write(fd, &one, 4)`: `1` unmasks the interrupt, `0` masks it (Linux
  `irqcontrol`). For level-triggered lines the kernel top half masks the
  source (IOAPIC RTE or GPIO pin), bumps the count and wakes readers; the
  driver clears the device's status and writes `1`.
* `poll()` - `POLLIN` when the count changed since the last read.
* `close()` / process exit - mask the IRQ, unmap, release the claim (3.5).

### 3.2 Device information (sysfs subset)

A small synthetic filesystem mounted at `/sys` (same mechanism as
`dev_dri_vfs.c` at `/dev/dri`), with the **Linux UIO sysfs layout**:

```
/sys/class/uio/uio0/name              "i2c-designware" / "edu" / ...
/sys/class/uio/uio0/version           "inDevOS-1"
/sys/class/uio/uio0/event             interrupt count
/sys/class/uio/uio0/maps/map0/{name,addr,size,offset}
/sys/class/uio/uio0/portio/port0/{name,start,size,porttype}
/sys/class/uio/uio0/device/{modalias,resources,irq}   inDevOS: source device
/sys/bus/pci/devices/0000:00:1f.3/{vendor,device,class,resource,irq}
/sys/bus/acpi/devices/PNP0C50:00/{hid,cid,uid,path,status,resources}
```

`resources` is a text dump of the decoded `_CRS` (Memory32Fixed,
I2cSerialBus, GpioInt, Interrupt) - from uACPI once present (section 6), from
`/etc/drivers.conf` overrides before that.

### 3.3 I/O ports

`ioperm(from, num, on)` and `iopl(level)` (Linux syscalls 173/172), root only,
and only for ranges that belong to a claimed device's port regions.
Implementation: a per-process 8 KiB I/O permission bitmap copied into the TSS
on context switch (or IOPL=3 in the saved RFLAGS for `iopl(3)`, which is
simpler but all-or-nothing). Not needed for the touchpad; needed for e.g. a
user-mode PS/2 or legacy device driver.

### 3.4 Claiming: `/dev/uioctl` (inDevOS extension)

Linux binds UIO via device tree / `new_id`; we have neither, so claiming is an
explicit ioctl on a control device (root only):

```c
struct uioctl_claim {
    char     bus[8];       /* "pci" | "acpi" | "raw" */
    char     id[64];       /* "0000:00:1f.3" | "PNP0C50:00" | name for raw */
    uint32_t flags;        /* UIOCTL_IRQ_MSI, UIOCTL_WC, UIOCTL_NO_IRQ ...  */
    /* raw only (manual resources until AML works): */
    uint64_t mmio_base[4], mmio_len[4];
    uint32_t irq;          /* GSI, or UIOCTL_IRQ_GPIO|pin                    */
    uint32_t irq_flags;    /* level/edge, active low/high                    */
    int32_t  uio_minor;    /* out: N of /dev/uioN                            */
};
#define UIOCTL_CLAIM   _IOWR('U', 1, struct uioctl_claim)
#define UIOCTL_RELEASE _IOW ('U', 2, int)
```

The kernel refuses a claim when a kernel driver already owns the device
(`driver_manager` state, or the PCI function is bound to ahci/nvme/xhci/...),
when ranges overlap RAM, the kernel image, the framebuffer or another claim,
or when a raw range is not inside an ACPI/PCI resource (once uACPI exists).
For PCI claims bus mastering stays **off** unless `UIOCTL_BUSMASTER` is given
(see 3.5).

### 3.5 Safety and permissions

* Drivers run as uid 0 started by `drvd`; ordinary users cannot open
  `/dev/uio*`, `/dev/uioctl` or `/dev/uinput`.
* No IOMMU is set up, so a user driver with bus-mastering DMA can write all
  of RAM. v1 allows **no DMA** (I2C-HID needs none). Later: a `udmabuf`-style
  allocator for pinned buffers returning physical addresses, root only, and
  AMD-Vi (IVRS table) to confine devices.
* On exit/crash the kernel masks the IRQ, unmaps, releases the claim, and
  destroys the process's uinput devices **after emitting release events for
  every key/button still down** (no stuck buttons).

### 3.6 Plumbing note

Rather than adding `/dev/uio`, `/dev/uioctl`, `/dev/uinput`, `/dev/input/*` as
four more `fd_type` branches in read/write/ioctl/poll/close/fstat/mmap, add one
`FD_TYPE_CHRDEV` with a `const struct chrdev_ops *` (`open/read/write/ioctl/
poll/mmap/release`) and a `void *priv` in the fd. `/dev/fb0` can move onto it
later.

---

## 4. Interrupts (kernel IRQ core)

Needed for UIO and also fixes existing problems:

1. **Vector allocator** for 64-239 and a table of generic stubs, plus
   `irq_request(gsi_or_msi, flags, handler, ctx)` / `irq_free()`. Sharing for
   level-triggered lines (handler returns handled/not-handled).
2. **Trigger/polarity from the source**: MADT ISO, ACPI `Interrupt()`
   descriptor flags, `GpioInt` flags; PCI INTx is level/active-low. Today
   everything without an ISO is programmed edge/active-high.
3. **Mask/unmask** per source for UIO's level-triggered protocol.
4. MSI/MSI-X via the existing `pci_msi_enable()` with an allocated vector.
5. **AMD GPIO demux (kernel)**. The AMD GPIO controller (`AMDI0030`, MMIO at
   about `0xFED81500`, one shared GSI, often 7, level/low) multiplexes the
   interrupts of the touchpad, lid, power button and others. It cannot belong
   to one user process, so a small kernel irqchip (a subset of Linux
   `pinctrl-amd`: per-pin 32-bit registers at `base + 4*pin`, bits for level
   trigger (8), active level (9-10), interrupt enable (11), mask (12),
   status (28), and the master EOI bit at `0xFC`) turns each pin into its own
   IRQ source that UIO can hand out (`UIOCTL_IRQ_GPIO|pin`).

---

## 5. Input: evdev + uinput

### 5.1 Kernel input core

```c
struct input_dev *input_allocate(name, bustype, vendor, product);
input_set_capability(dev, EV_KEY, BTN_LEFT); input_set_abs_params(...);
input_register(dev);                          /* creates /dev/input/eventN */
input_event(dev, EV_REL, REL_X, dx); input_sync(dev);
```

Sources: PS/2 keyboard (set-1 scancode -> `KEY_*`), PS/2 mouse
(`REL_X/Y/WHEEL`, `BTN_LEFT/RIGHT/MIDDLE`), USB HID boot keyboard and mouse,
and uinput devices. Existing sinks become **input handlers** on the core:

* **pointer handler** - consumes `EV_REL`/`BTN_*` from every pointer device
  (and `EV_ABS` from absolute pointers such as QEMU's usb-tablet) and drives the
  existing `mouse_x/mouse_y/mouse_buttons`, `luade_mouse_event()`, SDL
  accumulators and the scroll accumulator. `mouse_feed_usb_relative()` becomes
  this handler's internal function, gaining a wheel argument.
* **keyboard handler** - `EV_KEY` from uinput keyboards is turned back into
  set-1 scancodes and fed to `keyboard_inject_scancode()`, so VTs, termios,
  Ctrl-C and layouts keep one code path. The real PS/2 and USB keyboards keep
  feeding the scancode path directly *and* report to the input core.
* **evdev handler** - per-open client ring of `struct input_event`.

### 5.2 `/dev/input/eventN` (Linux evdev ABI)

* `read()` returns `struct input_event` (24 bytes on x86-64: `timeval`, `u16
  type`, `u16 code`, `s32 value`), `SYN_REPORT` framing, `SYN_DROPPED` on
  overflow.
* ioctls: `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`, `EVIOCGPHYS`,
  `EVIOCGUNIQ`, `EVIOCGPROP`, `EVIOCGBIT(ev, len)`, `EVIOCGKEY`, `EVIOCGABS`,
  `EVIOCGRAB` (exclusive grab: the pointer/keyboard handlers stop seeing that
  device while grabbed - this is how a full-screen game would take the mouse),
  `EVIOCSCLOCKID`.
* `poll()`; `/dev/input/by-id` is not needed. Optional `/dev/input/mice`
  (PS/2 protocol multiplexer) for very old programs.
* Readable by root and group `input` (simple: root and the console user).

### 5.3 `/dev/uinput` (Linux uinput ABI)

* `UI_SET_EVBIT`, `UI_SET_KEYBIT`, `UI_SET_RELBIT`, `UI_SET_ABSBIT`,
  `UI_SET_PROPBIT`, `UI_DEV_SETUP` (`struct uinput_setup`), `UI_ABS_SETUP`,
  legacy `write(struct uinput_user_dev)`, `UI_DEV_CREATE`, `UI_DEV_DESTROY`,
  `UI_GET_SYSNAME`, `UI_GET_VERSION` (5).
* `write(struct input_event[])` after `UI_DEV_CREATE` injects events.
* The created device is an ordinary input device: it gets an `eventN` node and
  feeds the pointer/keyboard handlers.
* Destroyed on close/exit with release events (3.5).

### 5.4 What the touchpad driver emits (decision)

inDevOS has no libinput, and the desktop lives in the kernel. So:

* **Device A, "ELAN touchpad"**: the real multitouch data, Linux-style
  (`INPUT_PROP_POINTER`, `INPUT_PROP_BUTTONPAD`, `ABS_MT_SLOT`,
  `ABS_MT_TRACKING_ID`, `ABS_MT_POSITION_X/Y`, `ABS_X/Y`, `BTN_TOUCH`,
  `BTN_TOOL_FINGER..QUINTTAP`, `BTN_LEFT`). This is marked as a touchpad, so
  the pointer handler **ignores** it; apps that understand touchpads can read it.
* **Device B, "touchpad pointer"**: `REL_X/Y`, `REL_WHEEL(_HI_RES)`,
  `REL_HWHEEL`, `BTN_LEFT/RIGHT/MIDDLE`. The driver does gestures (motion with
  acceleration, tap-to-click, two-finger scroll, two/three-finger tap = right/
  middle, clickpad bottom-right = right button, palm/thumb rejection) and
  emits ordinary mouse events, which the desktop already understands.

This keeps gesture policy out of the kernel and in a file that can be changed
and restarted without rebooting.

---

## 6. ACPI information and the AML question

The touchpad driver needs, from the DSDT/SSDTs:

| Item | Where in ACPI | Example |
|---|---|---|
| Touchpad device present | `_HID` (e.g. `ELAN0001`, `SYNA2B5C`, `MSFT0001`), `_CID` `PNP0C50`, `_STA` | `\_SB.I2CA.TPD0` |
| I2C slave address, speed | `_CRS` -> `I2cSerialBusV2(0x15, ..., 400000, ..., "\_SB.I2CA")` | 0x15 @ 400 kHz |
| Which controller | the `I2cSerialBus` resource source -> that device's `_HID` `AMDI0010`/`AMDI0019`/`AMDI0510` and its `_CRS` `Memory32Fixed` | `0xFEDC3000`, 4 KiB |
| Interrupt | `_CRS` -> `GpioInt(Level, ActiveLow, ..., "\_SB.GPIO") {pin}` (or `Interrupt()` with a GSI) | pin 9, level low |
| HID descriptor register | `_DSM(3CDFF6F7-4267-4555-AD05-B30A3D8938DE, rev 1, func 1)` | usually `0x0001` or `0x0020` |
| Power-up | `_PS0`/`_PS3` of the controller and touchpad, sometimes `_INI`; FCH AOAC power gating on AMD | |
| Controller clock | not in ACPI; Linux hardcodes it per HID in `acpi_apd.c` (133 MHz for `AMDI0010` as far as I know; to confirm against the source) | |

On real laptops `_CRS` and `_DSM` are frequently **methods** that select
between touchpad variants with NVS variables (`TPTY`, `TPDT`...), and `_STA`
often depends on OS variables. A byte scan can find candidates but cannot
evaluate them reliably.

**Decision:**

* **Now (phase 0/5a):** no interpreter. `acpidump` brings the tables back,
  we decompile with `iasl` on the PC, read off the address, controller base,
  GPIO pin and HID descriptor register for *this* laptop, and put them in
  `/etc/drivers.conf` as a manual override. Enough to bring the touchpad up.
* **Then (phase 6): integrate uACPI** (MIT licence, written for hobby kernels,
  small host API: map/unmap, PCI config, port I/O, mutex/event/spinlock,
  sleep/stall, interrupt install for the SCI, allocation). It gives `_STA`,
  `_CRS` decoding, `_DSM`, `_PS0`, namespace walk for discovery, and fixes
  S5/reboot, power button, lid, EC and battery as a bonus. ACPICA is the
  alternative (GPL/BSD dual, larger, older host API); uACPI is the better fit.
  Generic support for "any laptop" really requires this.

---

## 7. Discovery and launch

### 7.1 `/etc/drivers.conf`

```ini
# match = bus:id with shell-style wildcards; first match wins
[touchpad]
match   = acpi:PNP0C50          # _CID, or acpi:ELAN*, acpi:SYNA*, acpi:MSFT0001
exe     = /sbin/drv/i2chid.exe
args    = --gestures=default
restart = always                # always | on-failure | never

# manual resources (used until uACPI decodes _CRS; per machine)
i2c_ctrl     = mmio:0xFEDC3000+0x1000,hid:AMDI0010,clock:133000000
i2c_addr     = 0x15
i2c_speed    = 400000
hid_desc_reg = 0x0001
irq          = gpio:9,level,low  # or gsi:N,level,low   or poll:8ms

[edu-test]
match = pci:1234:11e8
exe   = /sbin/drv/edutest.exe
```

### 7.2 `drvd.exe` (supervisor, user mode)

Started by the kernel after the root filesystem is mounted (same mechanism as
the desktop's `busybox sh`), runs as uid 0.

1. Enumerate `/sys/bus/pci/devices` and `/sys/bus/acpi/devices` (before uACPI:
   the kernel lists `_HID`/`_CID` strings it finds by byte scan, plus
   `[name]` sections with manual resources).
2. Match against `drivers.conf`; skip devices a kernel driver owns.
3. Claim via `/dev/uioctl`, `fork`+`exec` the driver with
   `INDEVOS_UIO=/dev/uio0` and `INDEVOS_DEV=acpi:PNP0C50:00` in the
   environment (the driver may also claim itself).
4. `waitpid()`: on crash restart with backoff 0.5 s, 1 s, 2 s ... 30 s; give up
   after 5 crashes in 60 s and log it. stdout/stderr go to
   `/var/log/drv-<name>.log` and serial.
5. Control from the shell: a `drv` command (`drv list`, `drv restart
   touchpad`, `drv stop touchpad`, `drv log touchpad`) talking to `drvd` over a
   Unix socket or FIFO (`/run/drvd.ctl`).

While `drvd` is not written, a driver can be started by hand from the shell
(`/sbin/drv/i2chid.exe &`), which is how the first tests will run.

---

## 8. The touchpad stack

### 8.1 Split

| Layer | Where | Why |
|---|---|---|
| AMD GPIO interrupt demux | **kernel** (~300 lines) | Shared by lid/power button/touchpad; one shared GSI. |
| DesignWare I2C controller (`AMDI0010`) | **user**, in `i2chid.exe` (`libdwi2c`) | The touchpad is normally alone on its bus; MMIO only, no DMA. |
| I2C-HID protocol | **user** (`libi2chid`) | Pure protocol over an `i2c_xfer()` callback. |
| HID report parser + Precision Touchpad / mouse mapping | **user** (`libhid`) | Large, bug-prone, benefits most from crash isolation. |
| Gestures -> uinput | **user** | Policy; restartable. |

One executable with three internal libraries behind small interfaces
(`i2c_xfer(addr, wbuf, wlen, rbuf, rlen)`). If a second I2C device on the same
bus ever matters (touchscreen, sensor hub), `libdwi2c` moves into the kernel
as a real `/dev/i2c-N` (Linux `i2c-dev` ABI, `I2C_RDWR`) and `i2chid.exe` uses
that instead, without touching the other two libraries.

### 8.2 Bring-up sequence

1. Map the controller MMIO; read `IC_COMP_TYPE` (offset `0xFC`, must read
   `0x44570140`) and `IC_COMP_PARAM_1` - a safe, read-only check that the
   base address is right before writing anything.
2. If the controller is power-gated (reads all `0xFFFFFFFF`), it needs `_PS0`
   or AMD FCH AOAC power-on - one of the reasons for uACPI.
3. DesignWare init: `IC_ENABLE=0`, `IC_CON` = master, fast mode, restart
   enable, 7-bit; `IC_FS_SCL_HCNT/LCNT` from the clock (133 MHz on
   `AMDI0010`), SDA hold, `IC_TAR=addr`, mask all interrupts (polled
   transfers at first), `IC_ENABLE=1`.
4. I2C-HID: read the 30-byte HID descriptor at `hid_desc_reg`; check
   `wHIDDescLength=30`, `bcdVersion=0x0100`. Read the report descriptor
   (`wReportDescRegister`, `wReportDescLength`). `SET_POWER(ON)`, `RESET` and
   wait for the 2-byte zero-length reset ack (on the interrupt, or poll).
5. Parse the report descriptor. For a Windows Precision Touchpad
   (usage page 0x0D, usage 0x05), `SET_REPORT(Feature, Input Mode = 3)` to
   leave mouse-emulation mode; read Contact Count Maximum and the
   button-type (clickpad) feature.
6. Loop: wait for the IRQ (`read(/dev/uioN)`) or poll every ~8 ms; read
   `wMaxInputLength` from `wInputRegister`; length 0 = nothing; decode,
   update contacts, run gestures, write uinput events; `write(uio, 1)` to
   unmask.

**Polling mode** (no IRQ, `irq = poll:8ms`) is supported from the start so the
I2C and HID parts can be brought up before the GPIO demux exists. It costs one
short I2C read every 8 ms.

Note: many laptop touchpads also have a PS/2 emulation mode wired to the EC.
If the laptop's touchpad already moves the cursor in inDevOS through the PS/2
mouse path, the I2C driver is about *gestures and precision*, not about having
a pointer at all - worth checking first (open question 1).

---

## 9. Implemented now: `acpidump`

Zeus shell command (kernel, `src/acpidump.c`):

```
acpidump            # write to /acpi on the root mount
acpidump /mnt/acpi  # another directory
acpidump -h         # only print table list + hints, write nothing
```

Writes, as 8.3 names:

* `DSDT.AML`, `SSDT1.AML` ... `SSDTn.AML`, `FACP.AML`, `APIC.AML`,
  `FACS.AML` and every other table in the RSDT/XSDT - raw bytes, exactly what
  `iasl` expects.
* `TABLES.TXT` - signature, length, revision, physical address, OEM IDs,
  checksum status of every table.
* `PCI.TXT` - every PCI function: `bus:dev.fn vendor:device class irq BARs`
  (also answers the laptop's GPU ID question).
* `HINTS.TXT` (also printed on screen) - a read-only byte scan of the
  DSDT/SSDTs for touchpad-related IDs (`PNP0C50`, `AMDI0010/0019/0030`,
  `ELAN`, `SYNA`, `MSFT0001`, ...), the I2C-HID `_DSM` UUID, and every
  `I2cSerialBus` and `GpioInt` resource descriptor with address, speed, pin,
  polarity, controller path, and the enclosing `Device` name. No AML is run.

Booted from the live USB stick, `/` is the stick's FAT32 partition, so the
files end up on the stick. On Windows that partition shows up as a normal
drive; then:

```
iasl -e SSDT*.AML -d DSDT.AML     # DSDT with external references resolved
iasl -d SSDT*.AML
```

Tested in QEMU (q35, BIOS, os_install.iso booted as a USB stick on xHCI) with a
fake touchpad SSDT injected via `-acpitable` (`ELAN0001`/`PNP0C50` under an
`AMDI0010` controller, `I2cSerialBusV2(0x15, 400 kHz)`, `GpioInt(Level,
ActiveLow, pin 9, "\_SB.GPIO")`, HID `_DSM`): all 8 tables were written, the
SSDT came back byte-identical, `iasl -d` decompiled the DSDT and SSDT, and the
hints listed every planted item with the right device names.

---

## 10. Phased plan

| Phase | Content | Size (new code) | Effort |
|---|---|---|---|
| 0 | `acpidump` + hints (**done**) | ~450 lines | - |
| 1 | Input core, `/dev/input/eventN` for PS/2 + USB kbd/mouse, pointer/keyboard handlers replace direct calls, wheel for USB mice, `FD_TYPE_CHRDEV` ops table, `evdump.exe` test tool | ~1200 | 3-5 days |
| 2 | `/dev/uinput`, cleanup on exit, `fakemouse.exe` test driver | ~600 | 2-3 days |
| 3 | IRQ core (vector allocator, generic stubs, trigger/polarity, mask/unmask, sharing, MSI), UIO core (`/dev/uioN`, `/dev/uioctl`, `/sys` subset), `ioperm/iopl` | ~1500 | 1-1.5 weeks |
| 4 | `drvd.exe`, `/etc/drivers.conf`, `drv` command, restart/backoff | ~800 | 3-4 days |
| 5a | `i2chid.exe` in polling mode with manual config from the dumped DSDT: DesignWare, I2C-HID, HID parser, PTP mode, gestures | ~2000 | 1-2 weeks, on hardware |
| 5b | AMD GPIO kernel demux, switch the touchpad to its GPIO interrupt | ~350 | 2-3 days |
| 6 | uACPI integration: namespace discovery, `_STA/_CRS/_DSM/_PS0`, `/sys/bus/acpi`; drop manual config; S5, power button, lid | ~1500 glue + uACPI | 1-2 weeks |

Suggested order: 0 -> 1 -> 2 -> 3 (minimal raw claim + polled UIO is enough
to start 5a) -> 5a -> 5b -> 4 -> 6. Phases 1-2 are useful on their own:
SDL/games and evdev tools work, and you can test the whole input path in
QEMU.

---

## 11. Testing in QEMU

QEMU on x86 has no DesignWare I2C controller and no I2C-HID device, so the
hardware-facing part is tested on the laptop, and everything else in QEMU:

* **evdev**: PS/2 keyboard/mouse, `-device usb-kbd`, `usb-mouse`,
  `usb-tablet` (absolute); `evdump.exe` (or a static `evtest` build) checks
  names, capability bits and events; QMP `input-send-event` drives it from the
  test script.
* **uinput**: `fakemouse.exe` moves the cursor in a circle and clicks; a
  screenshot checks the desktop cursor; `evdump.exe` sees the new device; kill
  it mid-click and check the button is released.
* **UIO + IRQ**: QEMU's `-device edu` (PCI `1234:11e8`): MMIO BAR, a
  "raise interrupt" register (`0x60`), interrupt status/ack (`0x24`/`0x64`),
  INTx and MSI. `edutest.exe` claims it, maps BAR0, checks the ID register,
  raises interrupts and counts them through `read()`/`write(1)`, both INTx
  (level, exercising mask/unmask) and MSI. `ivshmem` can test large mmaps.
* **Supervisor**: a driver that crashes on purpose; check restart, backoff and
  give-up.
* **ACPI parsing**: fake SSDTs via `-acpitable file=x.aml` (as done for
  `acpidump`); later the real laptop DSDT/SSDTs themselves can be loaded into
  QEMU the same way to test uACPI's evaluation of the touchpad's `_CRS`/
  `_DSM`/`_STA` methods without the laptop.
* **I2C-HID + HID parser + gestures**: built against a fake `i2c_xfer()` that
  replays a recorded HID descriptor, report descriptor and input reports
  (captured on the laptop by `i2chid.exe --record`, or from Linux
  `/sys/bus/hid/devices/*/report_descriptor`). Runs in QEMU and even on the
  Linux/WSL host as a unit test.
* **DesignWare**: on the laptop, starting with the read-only
  `IC_COMP_TYPE` probe and verbose serial logs.

---

## 12. Open questions

1. Does the laptop's touchpad move the cursor in inDevOS today (PS/2
   emulation via the EC)? If yes, we already have a fallback pointer.
2. Exact touchpad (`_HID`), I2C controller (`AMDI0010` vs `AMDI0019`), its
   MMIO base, the slave address, GPIO pin and HID descriptor register: answered
   by `acpidump` on the laptop.
3. Is the I2C controller powered on by the firmware, or does it need
   `_PS0`/AOAC first (registers reading `0xFFFFFFFF`)?
4. Can Georgios boot a Linux live USB on the laptop once? `sudo acpidump`,
   `ls /sys/bus/i2c/devices`, and the touchpad's `report_descriptor` would
   remove most guesswork and give a recording for the replay tests.
5. uACPI vs keeping manual per-machine config for a while longer.
6. Should `/sys` be a real (small) synthetic filesystem now, or should the
   first version use ioctls on `/dev/uioN` and add `/sys` later?
7. DMA for future user-mode drivers (NIC, audio): wait for AMD-Vi, or allow
   trusted root drivers to use pinned buffers without an IOMMU?
