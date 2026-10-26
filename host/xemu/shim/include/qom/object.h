#ifndef QOM_OBJECT_H
#define QOM_OBJECT_H

#include "qemu/osdep.h"

struct ObjectClass {
    const char *type_name;
};

struct Object {
    ObjectClass *klass;
};

typedef enum ResetType { RESET_TYPE_COLD } ResetType;

typedef struct ResettablePhases {
    void (*enter)(Object *obj, ResetType type);
    void (*hold)(Object *obj, ResetType type);
    void (*exit)(Object *obj, ResetType type);
} ResettablePhases;

typedef struct ResettableClass {
    ResettablePhases phases;
} ResettableClass;

typedef struct InterfaceInfo {
    const char *type;
} InterfaceInfo;

typedef struct TypeInfo {
    const char *name;
    const char *parent;
    size_t instance_size;
    void (*instance_init)(Object *obj);
    void (*class_init)(ObjectClass *klass, const void *data);
    InterfaceInfo *interfaces;
} TypeInfo;

void type_register_static(const TypeInfo *info);
#define type_init(fn)                                                                                                  \
    static void __attribute__((constructor)) xbrt_type_init_##fn(void) { fn(); }

ResettableClass *xbrt_resettable_class(ObjectClass *klass);

#define OBJECT(obj) ((Object *)(obj))
#define OBJECT_CHECK(type, obj, name) ((type *)(obj))
#define RESETTABLE_CLASS(klass) xbrt_resettable_class((ObjectClass *)(klass))

typedef struct MachineState MachineState;
#define MACHINE(obj) ((MachineState *)(obj))
Object *qdev_get_machine(void);
Object *object_dynamic_cast(Object *obj, const char *typename);
void object_unparent(Object *obj);

#endif
