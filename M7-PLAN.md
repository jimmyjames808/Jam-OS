# M7 plan: USB, keyboard and mouse, console, shell

Goal (ARCHITECTURE.md milestone row): **typing into the shell on the real PC
with the USB drivers as processes; killing the HID driver mid-use recovers;
`ktest` runs from the shell.** Everything is a process from the start (the
2026-09-29 migration rule); the kernel only enforces.

The PC's USB (NEXT.md "PC facts"): one Intel xHCI 8086:7A60 (MSI only, 8
vectors, 25 ports, 34 scratchpads; the M6 no-op driver works on it), an
ASMedia USB 3 hub 174C:2074 (USB 2 half) / 174C:3074 (USB 3 half), and
composite HID devices: Cooler Master 2516:01C9 / 01C1, Sino Wealth
258A:0033, Microdia 0C45:652F. The keyboard is probably behind the hub, so
a full/low-speed device behind a high-speed hub: the Transaction Translator
fields in the slot context must be right, or the keyboard never enumerates.

## What M7 adds

| Piece | What it is |
|---|---|
| usb-bus (process) | the xHCI driver grown from xhci-noop: port reset and speed, slots and contexts, Address Device, control transfers, configuration, interrupt IN endpoints, events, hot-plug, **hubs** (hub class handled inside usb-bus, as Linux's usbcore does: hubs are bus topology, not a class device), TT routing for full/low-speed devices behind high-speed hubs. Serves the `usb` protocol to class drivers |
| HID (process) | binds to HID interfaces (class 3), boot protocol (subclass 1: protocol 1 keyboard, 2 mouse) on each interface of a composite device, SET_PROTOCOL(boot) + SET_IDLE, interrupt-IN reports -> `input` events |
| Keyboard layer | in the HID process: HID usages -> key codes, modifiers, US layout, key repeat, Ctrl+Alt+Del |
| Console (process) | owns the framebuffer after boot (a WC physical VMO of it); text terminal with scrollback, the kernel log mirrored into it, `console` protocol for programs (write, read a line / keys) |
| Shell (process) | line editing, history, commands: `help`, `devices`, `usb`, `ps`, `run <prog>`, `ktest [prefix]`, `bench`, `stress <s>`, `reboot`, `mem`, `log` |
| Kernel additions | klog read (a reader handle so the console can follow the kernel log), framebuffer hand-off (fbcon stops drawing once the console owns the screen; panic takes it back), `debug_command` (run ktest / bench / stress from a program, needs the root resource), `reboot` (ACPI reset register, then 0xCF9, then the keyboard controller; needs the root resource), COM1 receive interrupt as a second input source (tests in QEMU; and a spare keyboard on the PC if USB input breaks) |
| Supervision | devmgr restarts a driver process that dies (backoff, a limit); clients reconnect on PEER_CLOSED as their protocol defines (`input` and `console` define it) |
| IDL | handles in messages (the `usb` protocol passes VMOs; devmgr's hand-written protocol becomes IDL) |

## Fixed decisions

- **Hubs live in usb-bus.** Class drivers (HID now, mass storage in M8) see
  devices and interfaces, never ports or hubs. usb-bus reports attach and
  detach of interfaces to devmgr, which starts class drivers from a match
  table (class/subclass/protocol, or vendor/product).
- **The `usb` protocol** (usb-bus serves, class drivers call; abi/idl/usb.idl):
  `get_descriptor(dev, type, index, lang) -> bytes`,
  `control(dev, setup[8], data VMO or bytes) -> status, length`,
  `open_interrupt_in(dev, endpoint, packet size, count) -> ring VMO +
  event` (usb-bus fills a shared ring of reports; the class driver reads
  it, no message per report), `close_endpoint`, `set_configuration`,
  `set_interface`, and a device-gone signal. One channel per interface
  given to the class driver: it can only talk to its own interface.
- **Transfers are DMA into pinned VMOs** owned by usb-bus; class drivers
  never get a dma_cap. Report rings are separate VMOs mapped read-only
  into the class driver.
- **Interrupts**: the Intel xHCI is MSI-only, one vector; usb-bus uses
  interrupter 0 for everything in M7. IMOD tuned for keyboard latency
  (the no-op test's 40 us moderation is fine for input).
- **Enumeration order** (xHCI 1.2, USB 2.0 ch. 9): port connect ->
  reset (USB 2 ports: PORTSC.PR; USB 3 ports reset themselves) -> speed
  from PORTSC -> Enable Slot -> input context (slot: route string, speed,
  root port, for FS/LS behind a HS hub: parent hub slot + port + MTT; EP0
  max packet by speed) -> Address Device -> GET_DESCRIPTOR(device, 8) ->
  fix EP0 max packet (Evaluate Context) -> full device descriptor ->
  configuration descriptor(s) -> SET_CONFIGURATION -> Configure Endpoint
  for the interfaces' endpoints. Hubs: GET_DESCRIPTOR(hub), set the slot's
  Hub bit, number of ports, TT think time, then power ports, handle status
  change endpoint and port resets downstream.
- **Input events** (`input` protocol, abi/idl/input.idl): key events
  (key code, down/up/repeat, modifiers, the character in the layout),
  mouse events (dx, dy, wheel, buttons). HID pushes them to the console
  over a channel; the console is the only consumer in M7.
- **Layout**: US (the user is in Australia: AU keyboards are US layout).
- **Screen ownership**: the kernel draws until the console process asks
  for the framebuffer (a new syscall with the root resource, or devmgr
  hands it a RES_MMIO of the framebuffer + a "take the screen" call).
  From then on kernel log lines reach the screen through the console
  (klog reader). A panic draws directly again (it always wins). If the
  console dies, the kernel takes the screen back until it restarts.
- **Tests as commands**: the boot menu entries stay (they still work
  without USB), and the shell can run the same things: `ktest`, `bench`,
  `stress 120`, `devices`, `xhcitest` is retired (usb-bus supersedes it).
- **QEMU**: `usb-kbd`, `usb-mouse`, and `usb-hub` (a USB 2 hub, with a
  keyboard behind it) on qemu-xhci; tests type keys through the QEMU
  monitor (`sendkey`), so the whole path is tested headless.

## Tracks

Phase 1: a foundation commit (IDL with handles; usb/input/console IDL files;
driver.h additions; kernel syscalls stubbed), then four agents in parallel.

### Foundation (on main, before the agents)
- genidl: handle arguments/results; `drv_channel_call` variant carrying
  handles in `driver.h` (both builds); devmgr's protocol moved to IDL.
- abi/idl/usb.idl, input.idl, console.idl (the contracts).
- syscalls reserved: `klog_read`, `framebuffer_take/release`,
  `debug_command`, `reboot`; startup roles for the new services.
- QEMU flags: usb-kbd + usb-mouse, a usb-hub with a keyboard behind it.

### Track A: usb-bus (agent 1, the big one)
- Start from drivers/xhci-noop: keep its init; add port handling (USB 2 /
  USB 3 protocol ports from the Supported Protocol capabilities), slots,
  device/input contexts (CSZ 32/64), Address Device, control transfers on
  EP0 (Setup/Data/Status TRBs), Evaluate Context, Configure Endpoint,
  interrupt-IN rings, transfer events, port status changes, hot-plug and
  unplug, Disable Slot on detach.
- Hubs: hub descriptor, Hub bit + TT fields, port power, the hub's status
  change endpoint, downstream reset and enumeration, TT for FS/LS devices.
- Serve the `usb` protocol; report interfaces to devmgr.
- Report: a RESULTS line per device (`usb: 3-2 258a:0033 FS kbd+mouse
  behind hub 174c:2074`), and a `usb` shell command's data.
- QEMU tests: devices on root ports, behind a usb-hub, unplug/replug via
  the monitor (`device_del` / `device_add`), control transfer errors (STALL
  on an unknown request), a device that disappears mid-transfer.

### Track B: HID + keyboard layer (agent 2)
- Class driver process built against a **mock usb-bus** (a test server in
  utest that serves recorded descriptors and reports), so it doesn't wait
  for Track A. Real descriptors from the PC's devices to test composite
  handling: get them in the first PC run (Track A's RESULTS lines) — until
  then use QEMU's and typical boot-keyboard descriptors.
- Boot protocol keyboard + mouse, per interface; report parsing (8-byte
  keyboard report, rollover/phantom state, 3-4 byte mouse report),
  modifiers, US layout, key repeat (500 ms / 30 Hz), Ctrl+Alt+Del.
- `input` protocol to the console; reconnect when the console restarts.

### Track C: console + shell + kernel services (agent 3)
- Kernel: klog reader (a handle that reads the log from a position, with
  a signal on new lines), framebuffer hand-off, `debug_command` (ktest /
  bench / stress run in the kernel, output streamed back), `reboot`.
- COM1 receive: interrupt-driven input as a byte stream -> an `input`
  source (so QEMU tests and a serial terminal can type into the shell).
- Console process: framebuffer text (font from the kernel's 8x16, copied
  to user), scrollback, cursor, the kernel log, `console` protocol.
- Shell: line editor (left/right, backspace, history), the commands above.
- QEMU tests: type into the shell over COM1 (serial input) and check the
  output; `ktest m4` from the shell; `reboot` restarts QEMU (-no-reboot
  exits: the test sees it).

### Track D: supervision (agent 4, smaller)
- devmgr: restart a dead driver (exponential backoff 100 ms .. 5 s, give
  up after 5 in a minute and log it).
- **Stale DMA after a rebind (M6 phase 2 review finding 1, CONFIRMED:
  `ktest=review_m6p2_stale_dma_after_rebind` shows edu writing 4080 bytes
  into released pages).** devmgr must stop turning Bus Master Enable on at
  bind; the driver turns it on through its own dma_cap
  (`dma_cap_bus_master(dma, on)`, a new syscall + driver.h call) only after
  it has quiesced the device (xHCI: handoff, halt, HCRST; edu: DMA engine
  idle). Belt and braces: pins released by an unclean cap close stay
  quarantined until the next driver's dma_cap_bus_master(on) (Fuchsia's
  BTI quarantine). Make that ktest a normal test that passes.
- Handles a driver receives are not transferable (dma_cap, interrupt,
  BARs, device), so a driver can't smuggle them out through DR_SERVE; and
  a dma_cap's close turns BME off only if it is still the function's
  current cap (M6 phase 2 review finding 2).
- The config filter lets RIGHT_MANAGE holders (devmgr) change the power
  state, so a function left in D3 can be woken (review finding 7).
- The reconnect rule written into each protocol; a test driver that dies
  on command; utests for restart, backoff, give-up.

### Phase 2: join and the PC
- devmgr's match table: xHCI -> usb-bus; HID interfaces -> hid. init
  starts devmgr, console, shell on a plain boot.
- End-to-end in QEMU: sendkey -> shell prints it; kill hid mid-typing ->
  typing works again within a second.
- PC rounds: USB device list (first!), keyboard keys to the log, then the
  console + shell, then supervision.

## Done when
- QEMU at 4 and 8 CPUs: all ktests, init + utest, the end-to-end typing
  test through a hub, the kill-HID test, stress, crash tests.
- The real PC: the USB list shows every device (keyboard, mouse, hub,
  stick); typing into the shell works; `ktest` from the shell passes;
  killing the HID driver while typing recovers; `reboot` reboots; All
  tests, the 2-minute stress, and the 10-minute sign-off.

## Rules for the agents
- Work only in your worktree and your track's files; the foundation
  headers and IDL files are the contract. If one must change, say so in
  the report instead of changing another track's side.
- Drivers and services are processes; `<jam/driver.h>` only for drivers.
- Every commit leaves the tree building with all existing tests passing
  (4 and 8 CPUs in QEMU: `tools/qemu-test.sh`). Add tests for everything.
- No `Co-Authored-By` trailer on commits. Don't push, don't touch main,
  never write to a USB disk.
- Anything only the real PC can show must be listed in the report with
  what the PC run should print. Every wait bounded; a USB problem must
  never hang the kernel or the boot.
