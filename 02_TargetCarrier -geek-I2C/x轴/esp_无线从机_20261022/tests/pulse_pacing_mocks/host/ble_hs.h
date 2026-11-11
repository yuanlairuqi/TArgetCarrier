#include <stddef.h>
#define BLE_HS_CONN_HANDLE_NONE 0xffff
struct os_mbuf;
struct os_mbuf *ble_hs_mbuf_from_flat(const void*,size_t);
int ble_gatts_notify_custom(unsigned short,unsigned short,struct os_mbuf*);
