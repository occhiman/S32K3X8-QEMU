# S32K3X8-QEMU FlexCAN Integration (Instances 0..7)

## Overview
This repository now includes a FlexCAN QEMU device model and machine wiring for
all S32K358 FlexCAN controllers on `s32k3x8evb`:

- FlexCAN instance `0` at `0x40304000` (MB 0-31 IRQ `110`)
- FlexCAN instance `1` at `0x40308000` (MB 0-31 IRQ `114`)
- FlexCAN instance `2` at `0x4030C000` (MB 0-31 IRQ `117`)
- FlexCAN instance `3` at `0x40310000` (MB 0-31 IRQ `120`)
- FlexCAN instance `4` at `0x40314000` (MB 0-31 IRQ `122`)
- FlexCAN instance `5` at `0x40318000` (MB 0-31 IRQ `124`)
- FlexCAN instance `6` at `0x4031C000` (MB 0-31 IRQ `126`)
- FlexCAN instance `7` at `0x40320000` (MB 0-31 IRQ `128`)

Machine properties expose one CAN bus link per emulated controller:

- `canbus0` -> FlexCAN instance `0`
- `canbus1` -> FlexCAN instance `1`
- `canbus2` -> FlexCAN instance `2`
- `canbus3` -> FlexCAN instance `3`
- `canbus4` -> FlexCAN instance `4`
- `canbus5` -> FlexCAN instance `5`
- `canbus6` -> FlexCAN instance `6`
- `canbus7` -> FlexCAN instance `7`

## Build Steps
From the repository root:

```bash
cd qemu
./configure --target-list=arm-softmmu
ninja -C build
```

If you prefer `make`:

```bash
cd qemu
./configure --target-list=arm-softmmu
make -j"$(nproc)"
```

DMA integration is built automatically with the same commands above (no extra
configure flags required for `s32k3x8evb`).

## DMA Integration (eDMA)
The S32K358 eDMA model is integrated from the donor QEMU project into this tree:

- `qemu/hw/dma/s32k358_dma.c`
- `qemu/include/hw/dma/s32k358_dma.h`

Build/config integration points:

- `qemu/hw/dma/Kconfig`: adds `config S32K358_DMA`
- `qemu/hw/dma/meson.build`: compiles `s32k358_dma.c` when enabled
- `qemu/hw/arm/Kconfig`: `S32K3X8_MCU` selects `S32K358_DMA`

Board wiring (`s32k3x8evb`):

- eDMA registers mapped at `0x4020C000`
- eDMA TCD memory region 1 mapped at `0x40210000`
- eDMA TCD memory region 2 mapped at `0x40410000`
- DMA channel IRQs connected as `DMATCD0..31` -> NVIC lines `4..35`

Runtime note:

- No extra QEMU command-line option is required to enable DMA on
  `-M s32k3x8evb`; the device is instantiated by default during board init.

## Integration with Host CAN (Virtual)
Create eight Linux `vcan` interfaces:

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link add dev vcan1 type vcan
sudo ip link add dev vcan2 type vcan
sudo ip link add dev vcan3 type vcan
sudo ip link add dev vcan4 type vcan
sudo ip link add dev vcan5 type vcan
sudo ip link add dev vcan6 type vcan
sudo ip link add dev vcan7 type vcan
sudo ip link set vcan0 up
sudo ip link set vcan1 up
sudo ip link set vcan2 up
sudo ip link set vcan3 up
sudo ip link set vcan4 up
sudo ip link set vcan5 up
sudo ip link set vcan6 up
sudo ip link set vcan7 up
```

Run QEMU and connect one FlexCAN controller to one host interface:

```bash
./qemu-system-arm -object can-bus,id=canbus0   -object can-host-socketcan,id=hostcan0,if=vcan0,canbus=canbus0     -M s32k3x8evb,canbus0=canbus0 -cpu cortex-m7 -kernel ../../Demo/Firmware/inhibt-component.elf -nographic
```

Run QEMU and connect each FlexCAN controller to one host interface:

```bash
qemu/build/qemu-system-arm \
  -object can-bus,id=canbus0 \
  -object can-bus,id=canbus1 \
  -object can-bus,id=canbus2 \
  -object can-bus,id=canbus3 \
  -object can-bus,id=canbus4 \
  -object can-bus,id=canbus5 \
  -object can-bus,id=canbus6 \
  -object can-bus,id=canbus7 \
  -object can-host-socketcan,id=hostcan0,if=vcan0,canbus=canbus0 \
  -object can-host-socketcan,id=hostcan1,if=vcan1,canbus=canbus1 \
  -object can-host-socketcan,id=hostcan2,if=vcan2,canbus=canbus2 \
  -object can-host-socketcan,id=hostcan3,if=vcan3,canbus=canbus3 \
  -object can-host-socketcan,id=hostcan4,if=vcan4,canbus=canbus4 \
  -object can-host-socketcan,id=hostcan5,if=vcan5,canbus=canbus5 \
  -object can-host-socketcan,id=hostcan6,if=vcan6,canbus=canbus6 \
  -object can-host-socketcan,id=hostcan7,if=vcan7,canbus=canbus7 \
  -M s32k3x8evb,canbus0=canbus0,canbus1=canbus1,canbus2=canbus2,canbus3=canbus3,canbus4=canbus4,canbus5=canbus5,canbus6=canbus6,canbus7=canbus7 \
  -nographic \
  -kernel Demo/Firmware/firmware.elf
```

## Integration with Host CAN (Physical)
Example for one physical interface (`can0`):

```bash
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
```

Then connect one or more emulated controllers to that host bus:

```bash
qemu/build/qemu-system-arm \
  -object can-bus,id=canbus0 \
  -object can-host-socketcan,id=hostcan0,if=can0,canbus=canbus0 \
  -M s32k3x8evb,canbus0=canbus0,canbus1=canbus0,canbus2=canbus0,canbus3=canbus0,canbus4=canbus0,canbus5=canbus0,canbus6=canbus0,canbus7=canbus0 \
  -nographic \
  -kernel Demo/Firmware/firmware.elf
```

This maps FlexCAN `0..7` onto the same host CAN network.

## MCAL Alignment Notes
The model was aligned for MCAL FlexCAN basic flows:

- MCR start/stop/freeze handshakes (`FRZACK`, `LPMACK`, `NOTRDY`)
- Message buffers `0..31` (classic layout at RAM offset `0x80`)
- `IMASK1`/`IFLAG1` interrupt flow with W1C flag clearing
- TX complete signaling through MB flags
- RX delivery from QEMU CAN bus into configured RX-empty mailboxes
- Internal self-reception/loopback behavior:
  - `CTRL1[LPB]=1`: transmitted frame is looped back locally (internal loopback)
  - `MCR[SRXDIS]=0`: self-reception is enabled in normal mode
  - `MCR[SRXDIS]=1`: self-reception is disabled in normal mode
  - In loopback mode, frames are not emitted on external `canbusX` links
- Legacy RX FIFO emulation (`MCR[RFEN]`, `IFLAG1[BUF5I/BUF6I/BUF7I]`, `RXFIR`)
- Enhanced RX FIFO emulation (`ERFCR`, `ERFIER`, `ERFSR`, output RAM at `0x2000`)

## Short MCAL Checklist (FlexCAN_Ip Mapping)
Use this checklist when setting up `Can_43_FLEXCAN` / `FlexCAN_Ip` controllers for this QEMU machine:

1. Configure controller instances as `0..7` (same numeric instance in QEMU and MCAL).
2. Route each controller to the matching machine property `canbusX`.
3. Enable MB interrupt line `0..31` (`FlexCANx_1_IRQn`) for interrupt-driven MB handling.
4. Use message buffers in `0..31` range for the currently modeled interrupt/data path.
5. Legacy and Enhanced RX FIFO paths are now emulated; for strict filter fidelity, prefer IDAM format A and validate your filter table assumptions.

| FlexCAN_Ip Instance | Base Address | QEMU Machine Link | MB 0-31 IRQ |
| --- | --- | --- | --- |
| `0` | `0x40304000` | `canbus0` | `FlexCAN0_1_IRQn` (`110`) |
| `1` | `0x40308000` | `canbus1` | `FlexCAN1_1_IRQn` (`114`) |
| `2` | `0x4030C000` | `canbus2` | `FlexCAN2_1_IRQn` (`117`) |
| `3` | `0x40310000` | `canbus3` | `FlexCAN3_1_IRQn` (`120`) |
| `4` | `0x40314000` | `canbus4` | `FlexCAN4_1_IRQn` (`122`) |
| `5` | `0x40318000` | `canbus5` | `FlexCAN5_1_IRQn` (`124`) |
| `6` | `0x4031C000` | `canbus6` | `FlexCAN6_1_IRQn` (`126`) |
| `7` | `0x40320000` | `canbus7` | `FlexCAN7_1_IRQn` (`128`) |

## Current Scope / Limitations
- Classic CAN payload path (up to 8 bytes)
- MB interrupt line `0..31` path
- No full CAN FD timing/data-path emulation
- Legacy FIFO filter decoding is exact for IDAM format A; IDAM B/C currently use permissive acceptance.
- Enhanced FIFO filter matching currently uses permissive acceptance (status, queueing, and interrupts are modeled).
- No error-state/bus-off behavioral fidelity beyond basic register interactions
