#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#include "../../user_io.h"
#include "../../shmem.h"
#include "../../hardware.h"
#include "../../lib/miniz/miniz.h"
#include "../minimig/minimig_a2065.h"
#include "xl1200.h"

extern int  ethernet_open(const char *iface, int promiscuous);
extern void ethernet_close(void);
extern void ethernet_send(const uint8_t *frame, int len);
extern int  ethernet_recv_nb(uint8_t *buf, int maxlen);
extern int  ethernet_set_mac_filter_mcast(const uint8_t *mac);
extern int  ethernet_macvlan_create(const char *parent, const char *name, const uint8_t *mac);
extern void ethernet_macvlan_delete(const char *name);
extern void ethernet_offload_off(const char *iface);
extern void ethernet_offload_on(const char *iface);

#define SB_BASE    0x1FF00000UL
#define SB_SIZE    0x10000UL
#define SB_MAGIC   0x0000
#define SB_TXW     0x0010
#define SB_TXR     0x0018
#define SB_RXW     0x0020
#define SB_RXR     0x0028
#define SB_MAC     0x0030
#define SB_TX      0x1000
#define SB_RX      0x5000
#define SB_SLOT    0x800
#define TX_RING    8
#define RX_RING    16
#define MAX_FRAME  (SB_SLOT - 8)
#define XL_MAGIC   0x584C455448303031ULL

#define OPT_NET       "[10:8]"
#define MACVLAN_NAME  "xl0"

static volatile uint8_t *mb;
static int link_mode = A2065_OFF;
static uint64_t station;
static unsigned long retry_timer;
static unsigned long rtc_timer;

static uint64_t rd64(uint32_t off) { return *(volatile uint64_t *)(mb + off); }
static void wr64(uint32_t off, uint64_t v) { *(volatile uint64_t *)(mb + off) = v; }

static void close_link()
{
	if (link_mode == A2065_OFF) return;
	ethernet_close();
	if (link_mode == A2065_MACVLAN) ethernet_macvlan_delete(MACVLAN_NAME);
	if (link_mode != A2065_TAP) ethernet_offload_on(link_mode == A2065_ETH1 ? "eth1" : "eth0");
	link_mode = A2065_OFF;
}

static void update_link()
{
	static int last_mode = A2065_OFF;
	int mode = user_io_status_get(OPT_NET);
	if (!mode) mode = A2065_ETH0;
	else if (mode == 1 || mode >= A2065_MODES) mode = A2065_OFF;

	uint64_t mac_word = rd64(SB_MAC);
	bool mac_changed = mac_word != station;
	station = mac_word;
	bool wired = mode == A2065_ETH0 || mode == A2065_ETH1;
	if (mode != last_mode || (mac_changed && mode == A2065_MACVLAN))
	{
		close_link();
		retry_timer = 0;
		last_mode = mode;
	}

	if (mode == A2065_OFF) return;
	uint8_t mac[6];
	for (int i = 0; i < 6; i++) mac[i] = station >> (40 - 8 * i);

	if (link_mode && mac_changed && wired) ethernet_set_mac_filter_mcast(mac);
	if (link_mode && mode != A2065_ETH1) return;
	if (!CheckTimer(retry_timer)) return;
	retry_timer = GetTimer(1000);
	if (!a2065_mode_available(mode))
	{
		close_link();
		return;
	}
	if (link_mode) return;

	const char *parent = mode == A2065_ETH1 ? "eth1" : "eth0";
	const char *iface = mode == A2065_TAP ? "tap0" : parent;
	if (mode == A2065_MACVLAN)
	{
		if (!(station >> 63) || !ethernet_macvlan_create(parent, MACVLAN_NAME, mac)) return;
		iface = MACVLAN_NAME;
	}

	if (!ethernet_open(iface, wired))
	{
		if (mode == A2065_MACVLAN) ethernet_macvlan_delete(iface);
		return;
	}
	link_mode = mode;
	if (mode != A2065_TAP) ethernet_offload_off(parent);
	if (wired && (station >> 63)) ethernet_set_mac_filter_mcast(mac);
	printf("[xl1200] Ethernet on %s\n", iface);
}

static void xl1200_poll()
{
	if (CheckTimer(rtc_timer))
	{
		rtc_timer = GetTimer(60000);
		user_io_send_rtc(2);
	}
	if (!mb || rd64(SB_MAGIC) != XL_MAGIC) return;

	update_link();
	static uint8_t frame[MAX_FRAME];
	uint32_t txw = rd64(SB_TXW);
	uint32_t txr = rd64(SB_TXR);
	if (txw != txr)
	{
		if (txw - txr > TX_RING) txr = txw;
		for (; txr != txw; txr++)
		{
			uint32_t slot = SB_TX + SB_SLOT * (txr % TX_RING);
			int len = rd64(slot) & 0x7FF;
			if (link_mode && len >= 14 && len <= MAX_FRAME)
			{
				memcpy(frame, (const void *)(mb + slot + 8), len);
				ethernet_send(frame, len);
			}
		}
		__sync_synchronize();
		wr64(SB_TXR, txr);
	}

	if (!link_mode) return;

	for (int n = 0; n < RX_RING; n++)
	{
		int len = ethernet_recv_nb(frame, MAX_FRAME);
		if (len <= 0) break;

		uint32_t rxw = rd64(SB_RXW);
		if (len < 14 || len > MAX_FRAME - 4 || rxw - (uint32_t)rd64(SB_RXR) >= RX_RING) continue;

		if (len < 60)
		{
			memset(frame + len, 0, 60 - len);
			len = 60;
		}
		uint32_t fcs = mz_crc32(0, frame, len);
		for (int k = 0; k < 4; k++) frame[len++] = fcs >> (8 * k);
		uint32_t hash = ~(uint32_t)mz_crc32(0, frame, 6) >> 26;

		uint32_t slot = SB_RX + SB_SLOT * (rxw % RX_RING);
		memcpy((void *)(mb + slot + 8), frame, len);
		__sync_synchronize();
		wr64(slot, len | (hash << 16));
		__sync_synchronize();
		wr64(SB_RXW, rxw + 1);
	}
}

char is_xl1200()
{
	static int type = 0;
	if (!type && user_io_get_core_name(1)[0])
	{
		type = !strcasecmp(user_io_get_core_name(1), "XL1200") ? 1 : 2;
		if (type == 1) core_poll = xl1200_poll;
	}
	return type == 1;
}

void xl1200_init()
{
	mb = (volatile uint8_t *)shmem_map(SB_BASE, SB_SIZE);
	if (mb) wr64(SB_MAGIC, 0);
	rtc_timer = GetTimer(60000);
}
