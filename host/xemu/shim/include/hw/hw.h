#ifndef HW_HW_H
#define HW_HW_H

#include "qapi/error.h"
#include "qom/object.h"

struct DeviceState {
    Object parent_obj;
};

typedef struct DeviceClass {
    ObjectClass parent_class;
    const char *desc;
    const VMStateDescription *vmsd;
} DeviceClass;

#define DEVICE(obj) ((DeviceState *)(obj))
#define DEVICE_CLASS(klass) ((DeviceClass *)(klass))

#endif
