/* SPDX-License-Identifier: GPL-2.0-only */
/* Narrow host services for the unmodified Linux btqca.c initialization code. */
#ifndef QBT_LINUX_H
#define QBT_LINUX_H
#include "qbt_internal.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Namespace the Linux services and btqca exports in the combined XNU image.
 * The source fetched by prepare-linux.py remains byte-identical upstream. */
#define vmalloc qbt_lx_vmalloc
#define vfree qbt_lx_vfree
#define kfree qbt_lx_kfree
#define kstrndup qbt_lx_kstrndup
#define strscpy qbt_lx_strscpy
#define msleep qbt_lx_msleep
#define bt_skb_alloc qbt_lx_bt_skb_alloc
#define kfree_skb qbt_lx_kfree_skb
#define skb_put qbt_lx_skb_put
#define skb_put_u8 qbt_lx_skb_put_u8
#define skb_pull_data qbt_lx_skb_pull_data
#define hci_recv_frame qbt_lx_hci_recv_frame
#define __hci_cmd_sync_ev qbt_lx_hci_cmd_sync_ev
#define __hci_cmd_sync qbt_lx_hci_cmd_sync
#define __hci_cmd_send qbt_lx_hci_cmd_send
#define __hci_reset_sync qbt_lx_hci_reset_sync
#define request_firmware qbt_lx_request_firmware
#define release_firmware qbt_lx_release_firmware
#define hci_set_fw_info qbt_lx_hci_set_fw_info
#define hci_set_quirk qbt_lx_hci_set_quirk
#define hci_set_msft_opcode qbt_lx_hci_set_msft_opcode
#define qca_check_bdaddr qbt_lx_qca_check_bdaddr
#define qca_set_bdaddr qbt_lx_qca_set_bdaddr
#define qca_set_bdaddr_rome qbt_lx_qca_set_bdaddr_rome
#define qca_read_soc_version qbt_lx_qca_read_soc_version
#define qca_uart_setup qbt_lx_qca_uart_setup
#define qca_send_pre_shutdown_cmd qbt_lx_qca_send_pre_shutdown_cmd
#define qca_set_speed qbt_lx_qca_set_speed
typedef uint8_t u8, __u8;
typedef uint16_t u16, __le16;
typedef uint32_t u32, __le32;
#define __packed __attribute__((packed))
#define IS_ENABLED(x) 1
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define cpu_to_le16(x) (x)
#define GFP_KERNEL 0
#define HCI_INIT_TIMEOUT 10000
#define HCI_EV_VENDOR 0xff
#define HCI_EV_CMD_COMPLETE 0x0e
#define HCI_EVENT_PKT 4
#define HCI_OP_READ_BD_ADDR 0x1009
#define HCI_QUIRK_USE_BDADDR_PROPERTY 1
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define min(a,b) ((a) < (b) ? (a) : (b))
typedef struct { u8 b[6]; } bdaddr_t;
static const bdaddr_t qbt_zero_address = {{0}};
#define BDADDR_ANY (&qbt_zero_address)
static inline int bacmp(const bdaddr_t *a, const bdaddr_t *b) { return memcmp(a,b,6); }
static inline void baswap(bdaddr_t *a, const bdaddr_t *b) { for(unsigned i=0;i<6;i++) a->b[i]=b->b[5-i]; }
struct qbt_device { struct qbt *q; };
struct hci_dev { struct qbt *q; struct qbt_device dev; bdaddr_t public_addr; };
struct sk_buff { struct qbt *q; u8 *data; size_t len, capacity; u8 type; u8 storage[300]; };
struct firmware { const u8 *data; size_t size; struct qbt *q; };
struct hci_event_hdr { u8 evt, plen; } __packed;
struct hci_ev_cmd_complete { u8 ncmd; u16 opcode; } __packed;
struct hci_rp_read_bd_addr { u8 status; bdaddr_t bdaddr; } __packed;
struct sk_buff *__hci_cmd_sync_ev(struct hci_dev *, u16, unsigned, const void *, u8, unsigned);
struct sk_buff *__hci_cmd_sync(struct hci_dev *, u16, unsigned, const void *, unsigned);
int __hci_cmd_send(struct hci_dev *, u16, unsigned, const void *);
int __hci_reset_sync(struct hci_dev *);
struct sk_buff *bt_skb_alloc(size_t, int);
void kfree_skb(struct sk_buff *);
void *skb_put(struct sk_buff *, size_t);
void skb_put_u8(struct sk_buff *, u8);
void *skb_pull_data(struct sk_buff *, size_t);
int hci_recv_frame(struct hci_dev *, struct sk_buff *);
#define hci_skb_pkt_type(s) ((s)->type)
int request_firmware(const struct firmware **, const char *, void *);
void release_firmware(const struct firmware *);
void *vmalloc(size_t);
void vfree(void *);
void kfree(void *);
char *kstrndup(const void *, size_t, int);
size_t strscpy(char *, const char *, size_t);
void msleep(unsigned);
void hci_set_fw_info(struct hci_dev *, const char *, ...);
void hci_set_quirk(struct hci_dev *, unsigned);
void hci_set_msft_opcode(struct hci_dev *, unsigned);
#define bt_dev_dbg(h,fmt,...) do { if (0) qbt_linux_log((h)->q,7,fmt,##__VA_ARGS__); } while (0)
#define bt_dev_info(h,fmt,...) qbt_linux_log((h)->q,6,fmt,##__VA_ARGS__)
#define bt_dev_err(h,fmt,...) qbt_linux_log((h)->q,3,fmt,##__VA_ARGS__)
#define BT_DBG(...) ((void)0)
#define BT_ERR(...) ((void)0)
#endif
