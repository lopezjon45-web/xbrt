#ifndef HW_PCI_H
#define HW_PCI_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "system/memory.h"

#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_REVISION_ID 0x08
#define PCI_CLASS_REVISION 0x08
#define PCI_CLASS_DEVICE 0x0A
#define PCI_SUBSYSTEM_VENDOR_ID 0x2C
#define PCI_SUBSYSTEM_ID 0x2E
#define PCI_INTERRUPT_PIN 0x3D
#define PCI_CONFIG_SPACE_SIZE 0x100

#define PCI_VENDOR_ID_NVIDIA 0x10DE
#define PCI_DEVICE_ID_NVIDIA_GEFORCE_NV2A 0x02A0
#define PCI_DEVICE_ID_NVIDIA_MCPX_APU 0x01B0
#define PCI_CLASS_DISPLAY_VGA 0x0300
#define PCI_CLASS_MULTIMEDIA_AUDIO 0x0401

#define PCI_BASE_ADDRESS_SPACE_MEMORY 0x00
#define PCI_BASE_ADDRESS_MEM_PREFETCH 0x08
#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))

#define TYPE_PCI_DEVICE "pci-device"
#define INTERFACE_CONVENTIONAL_PCI_DEVICE "conventional-pci-device"

struct PCIDevice {
    DeviceState qdev;
    uint8_t config[PCI_CONFIG_SPACE_SIZE];
    unsigned irq_line;
};

typedef struct PCIDeviceClass {
    DeviceClass parent_class;
    void (*realize)(PCIDevice *dev, Error **errp);
    void (*exit)(PCIDevice *dev);
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint16_t class_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
} PCIDeviceClass;

#define PCI_DEVICE(obj) ((PCIDevice *)(obj))
#define PCI_DEVICE_CLASS(klass) ((PCIDeviceClass *)(klass))

PCIDevice *pci_create_simple(PCIBus *bus, int devfn, const char *name);
void pci_register_bar(PCIDevice *dev, int region, uint8_t type, MemoryRegion *mr);
void pci_irq_assert(PCIDevice *dev);
void pci_irq_deassert(PCIDevice *dev);

static inline void pci_set_word(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void pci_set_long(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint16_t pci_get_word(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t pci_get_long(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

#endif
