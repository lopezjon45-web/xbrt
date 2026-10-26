#include <windows.h>

#include "shim.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"

typedef struct XbrtClass {
    PCIDeviceClass pci;
    ResettableClass resettable;
} XbrtClass;

#define MAX_TYPES 8
#define MAX_DEVICES 4
#define MAX_HANDLERS 8

static const TypeInfo *g_types[MAX_TYPES];
static unsigned g_type_count;
static PCIDevice *g_devices[MAX_DEVICES];
static unsigned g_device_count;

struct VMChangeStateEntry {
    VMChangeStateHandler *cb;
    void *opaque;
};
static VMChangeStateEntry g_handlers[MAX_HANDLERS];
static unsigned g_handler_count;

void type_register_static(const TypeInfo *info)
{
    if (g_type_count == MAX_TYPES) xbrt_refuse(XBRT_REFUSE_FEATURE, "QOM: more than %d types", MAX_TYPES);
    g_types[g_type_count++] = info;
}

ResettableClass *xbrt_resettable_class(ObjectClass *klass) { return &((XbrtClass *)klass)->resettable; }

PCIDevice *pci_create_simple(PCIBus *bus, int devfn, const char *name)
{
    (void)bus;
    (void)devfn;
    const TypeInfo *type = NULL;
    for (unsigned i = 0; i < g_type_count; i++)
        if (!strcmp(g_types[i]->name, name)) type = g_types[i];
    if (!type) xbrt_refuse(XBRT_REFUSE_FEATURE, "QOM: no type \"%s\" registered", name);
    if (!type->parent || strcmp(type->parent, TYPE_PCI_DEVICE))
        xbrt_refuse(XBRT_REFUSE_FEATURE, "QOM: \"%s\" is not a PCI device", name);
    if (g_device_count == MAX_DEVICES) xbrt_refuse(XBRT_REFUSE_FEATURE, "QOM: more than %d devices", MAX_DEVICES);

    XbrtClass *klass = g_malloc0(sizeof *klass);
    klass->pci.parent_class.parent_class.type_name = type->name;
    type->class_init(&klass->pci.parent_class.parent_class, NULL);

    PCIDevice *dev = g_malloc0(type->instance_size);
    dev->qdev.parent_obj.klass = &klass->pci.parent_class.parent_class;
    dev->irq_line = g_device_count;
    pci_set_word(dev->config + PCI_VENDOR_ID, klass->pci.vendor_id);
    pci_set_word(dev->config + PCI_DEVICE_ID, klass->pci.device_id);
    dev->config[PCI_REVISION_ID] = klass->pci.revision;
    pci_set_word(dev->config + PCI_CLASS_DEVICE, klass->pci.class_id);
    pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID, klass->pci.subsystem_vendor_id);
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID, klass->pci.subsystem_id);
    g_devices[g_device_count++] = dev;
    if (type->instance_init) type->instance_init(OBJECT(dev));

    Error *err = NULL;
    klass->pci.realize(dev, &err);
    if (err) xbrt_refuse(XBRT_REFUSE_FEATURE, "%s realize: %s", name, error_get_pretty(err));
    return dev;
}

unsigned xbrt_pci_device_count(void) { return g_device_count; }
PCIDevice *xbrt_pci_device(unsigned i) { return g_devices[i]; }

void pci_register_bar(PCIDevice *dev, int region, uint8_t type, MemoryRegion *mr)
{
    (void)type;
    xbrt_memory_add_bar(dev, region, mr);
}

void pci_irq_assert(PCIDevice *dev) { g_xbrt_host->irq_level(dev->irq_line, 1); }
void pci_irq_deassert(PCIDevice *dev) { g_xbrt_host->irq_level(dev->irq_line, 0); }

void xbrt_qom_reset_all(void)
{
    for (unsigned i = 0; i < g_device_count; i++) {
        ResettableClass *rc = RESETTABLE_CLASS(g_devices[i]->qdev.parent_obj.klass);
        if (rc->phases.hold) rc->phases.hold(OBJECT(g_devices[i]), RESET_TYPE_COLD);
    }
}

VMChangeStateEntry *qemu_add_vm_change_state_handler(VMChangeStateHandler *cb, void *opaque)
{
    if (g_handler_count == MAX_HANDLERS)
        xbrt_refuse(XBRT_REFUSE_FEATURE, "more than %d vm change state handlers", MAX_HANDLERS);
    g_handlers[g_handler_count] = (VMChangeStateEntry){cb, opaque};
    return &g_handlers[g_handler_count++];
}

void xbrt_runstate_start(void)
{
    for (unsigned i = 0; i < g_handler_count; i++) g_handlers[i].cb(g_handlers[i].opaque, true, RUN_STATE_RUNNING);
}
