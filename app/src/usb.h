#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * USB CDC ACM song upload protocol.
 *
 * Packet framing (both directions):
 *   [CMD/STATUS : 1 byte]
 *   [PAYLOAD_LEN : 4 bytes LE]
 *   [PAYLOAD : PAYLOAD_LEN bytes]
 *
 * Host→Device commands:
 *   0x01  PING            no payload                        → OK
 *   0x02  DISK_INFO       no payload                        → OK + header[512]
 *   0x03  DISK_FORMAT     no payload                        → OK
 *   0x04  SONG_BEGIN      name[24] + nblocks[4]             → OK + song_idx[2 LE]
 *   0x05  SONG_BLOCK      block_data[512]                   → OK
 *   0x06  SONG_COMMIT     no payload                        → OK
 *   0x07  SONG_REMOVE     song_idx[2 LE]                    → OK
 *   0x08  CATALOG_READ    no payload                        → OK + catalog[8×512]
 *   0x09  SONG_MULTIBLOCK count[2 LE] then count×512 raw   → OK (one ACK for all)
 *
 * Device→Host responses:
 *   0x00  OK   + optional payload
 *   0xFF  ERR  + optional ASCII error string
 */

#define USB_CMD_PING          0x01u
#define USB_CMD_DISK_INFO     0x02u
#define USB_CMD_DISK_FORMAT   0x03u
#define USB_CMD_SONG_BEGIN    0x04u
#define USB_CMD_SONG_BLOCK    0x05u
#define USB_CMD_SONG_COMMIT   0x06u
#define USB_CMD_SONG_REMOVE   0x07u
#define USB_CMD_CATALOG_READ    0x08u
#define USB_CMD_SONG_MULTIBLOCK 0x09u
#define USB_CMD_EXTCSD_DUMP     0x0Au  /* no payload → OK + ext_csd[512] */
#define USB_CMD_CODEC_DIAG      0x0Bu  /* no payload → OK + codec_diag[32] */
#define USB_CMD_READ_BLOCK      0x0Cu  /* block_addr[4 LE] → OK + data[512] (pauses audio) */
#define USB_CMD_WRITE_PROBE     0x0Du  /* block_addr[4 LE] → OK + result[1] (0=wfail 1=rfail 2=mismatch 3=ok); DESTRUCTIVE */
#define USB_CMD_WRITE_STRESS    0x0Eu  /* count[4 LE] → OK + first_fail[4 LE] (0xFFFFFFFF=all ok); DESTRUCTIVE */

#define USB_STATUS_OK         0x00u
#define USB_STATUS_ERR        0xFFu

bool usb_cdc_init(void);
bool usb_cdc_connected(void);
bool usb_upload_active(void);
void usb_cdc_poll(void);
