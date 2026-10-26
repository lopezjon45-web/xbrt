#ifndef QEMU_TYPEDEFS_H
#define QEMU_TYPEDEFS_H

typedef struct AddressSpace AddressSpace;
typedef struct CPUState CPUState;
typedef struct DeviceState DeviceState;
typedef struct DisplaySurface DisplaySurface;
typedef struct Error Error;
typedef struct GraphicHwOps GraphicHwOps;
typedef struct IRQState *qemu_irq;
typedef struct MemoryRegion MemoryRegion;
typedef struct MemoryRegionOps MemoryRegionOps;
typedef struct Object Object;
typedef struct ObjectClass ObjectClass;
typedef struct PCIBus PCIBus;
typedef struct PCIDevice PCIDevice;
typedef struct QEMUTimer QEMUTimer;
typedef struct QemuConsole QemuConsole;
typedef struct VMChangeStateEntry VMChangeStateEntry;
typedef struct VMStateDescription VMStateDescription;

#endif
