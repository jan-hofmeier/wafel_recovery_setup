#include <stddef.h>
#include <string.h>
#include <wafel/ios/svc.h>
#include <wafel/utils.h>

#include "bsp.h"

#define CROSS_PROCESS_HEAP_ID 0xcaff
#define BSP_IOCTL_WRITE       6
#define BSP_REQUEST_SIZE      0x260

/* /dev/bsp ioctl input, layout as built by MCP_bspWrite (0x0503d460) */
typedef struct {
    char     entity[0x20];      /* 0x00 */
    uint32_t instance;          /* 0x20 */
    char     attribute[0x20];   /* 0x24 */
    uint32_t size;              /* 0x44 */
    uint8_t  data[BSP_REQUEST_SIZE - 0x48];  /* 0x48, write data */
} BspRequest;

_Static_assert(offsetof(BspRequest, data) == 0x48, "BspRequest layout");
_Static_assert(sizeof(BspRequest) == BSP_REQUEST_SIZE, "BspRequest size");

int bspWrite(const char* entity, uint32_t instance, const char* attribute, uint32_t size, const void* buffer)
{
    if (size > sizeof(((BspRequest *)0)->data))
        return -1;

    int handle = iosOpen("/dev/bsp", 0);
    if (handle < 0) {
        return handle;
    }

    BspRequest *req = (BspRequest *)iosAlloc(CROSS_PROCESS_HEAP_ID, sizeof(BspRequest));
    if (!req) {
        iosClose(handle);
        return -1;
    }
    memset(req, 0, sizeof(*req));
    strncpy(req->entity, entity, sizeof(req->entity));
    req->instance = instance;
    strncpy(req->attribute, attribute, sizeof(req->attribute));
    req->size = size;
    memcpy(req->data, buffer, size);

    int res = iosIoctl(handle, BSP_IOCTL_WRITE, req, offsetof(BspRequest, data) + size, NULL, 0);

    iosFree(CROSS_PROCESS_HEAP_ID, req);
    iosClose(handle);

    return res;
}
