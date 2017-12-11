/*
 * Copyright 2012-2013 Luke Dashjr
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "compat.h"
#include "dynclock.h"
#include "fpgautils.h"
#include "icarus-common.h"
#include "miner.h"

#define CAIRNSMORE1_IO_SPEED 115200

// This is a general ballpark
#define CAIRNSMORE1_HASH_TIME 0.0000000024484

#define CAIRNSMORE1_MINIMUM_CLOCK  50
#define CAIRNSMORE1_DEFAULT_CLOCK  200
#define CAIRNSMORE1_MAXIMUM_CLOCK  210

struct device_drv cairnsmore_drv;

static void cairnsmore_drv_init();

static bool cairnsmore_detect_one(const char *devpath)
{
	struct ICARUS_INFO *info = calloc(1, sizeof(struct ICARUS_INFO));
	if (unlikely(!info))
		quit(1, "Failed to malloc ICARUS_INFO");

	info->baud = CAIRNSMORE1_IO_SPEED;
	info->work_division = 2;
	info->fpga_count = 2;
	info->quirk_reopen = false;
	info->Hs = CAIRNSMORE1_HASH_TIME;
	info->timing_mode = MODE_LONG;
	info->do_icarus_timing = true;

	if (!icarus_detect_custom(devpath, &cairnsmore_drv, info)) {
		free(info);
		return false;
	}
	return true;
}

static int cairnsmore_detect_auto(void)
{
	return serial_autodetect(cairnsmore_detect_one, "Cairnsmore1");
}

static void cairnsmore_detect()
{
	cairnsmore_drv_init();
	// Actual serial detection is handled by Icarus driver
	serial_detect_auto_byname(&cairnsmore_drv, cairnsmore_detect_one, cairnsmore_detect_auto);
}

static bool cairnsmore_send_cmd(int fd, uint8_t cmd, uint8_t data, bool probe)
{
	unsigned char pkt[64] =
		"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
		"vdi\xb7"
		"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
		"bfg0" "\xff\xff\xff\xff" "\xb5\0\0\0";
	if (unlikely(probe))
		pkt[61] = '\x01';
	pkt[32] = 0xda ^ cmd ^ data;
	pkt[33] = data;
	pkt[34] = cmd;
	return write(fd, pkt, sizeof(pkt)) == sizeof(pkt);
}

bool cairnsmore_supports_dynclock(int fd)
{
	if (!cairnsmore_send_cmd(fd, 0, 1, true))
		return false;
	if (!cairnsmore_send_cmd(fd, 0, 1, true))
		return false;

	uint32_t nonce = 0;
	{
		struct timeval tv_finish;
		struct thr_info dummy = {
			.work_restart = false,
			.work_restart_notifier = {-1, -1},
		};
		icarus_gets((unsigned char*)&nonce, fd, &tv_finish, &dummy, 1);
	}
	applog(LOG_DEBUG, "Cairnsmore dynclock detection... Got %08x", nonce);
	switch (nonce) {
		case 0x00949a6f:  // big    endian
		case 0x6f9a9400:  // little endian
			// Hashed the command, so it's not supported
			return false;
		default:
			applog(LOG_WARNING, "Unexpected nonce from dynclock probe: %08x", be32toh(nonce));
			return false;
		case 0:
			return true;
	}
}

#define cairnsmore_send_cmd(fd, cmd, data) cairnsmore_send_cmd(fd, cmd, data, false)

static bool cairnsmore_change_clock_func(struct thr_info *thr, int bestM)
{
	struct cgpu_info *cm1 = thr->cgpu;
	struct ICARUS_INFO *info = cm1->device_data;

	if (unlikely(!cairnsmore_send_cmd(cm1->device_fd, 0, bestM)))
		return false;

	// Adjust Hs expectations for frequency change
	info->Hs = info->Hs * (double)bestM / (double)info->dclk.freqM;

	dclk_msg_freqchange(cm1->proc_repr, 2.5 * (double)info->dclk.freqM, 2.5 * (double)bestM, NULL);
	info->dclk.freqM = bestM;

	return true;
}

static bool cairnsmore_init(struct thr_info *thr)
{
	struct cgpu_info *cm1 = thr->cgpu;
	struct ICARUS_INFO *info = cm1->device_data;
	struct icarus_state *state = thr->cgpu_data;

	if (cairnsmore_supports_dynclock(cm1->device_fd)) {
		info->dclk_change_clock_func = cairnsmore_change_clock_func;

		dclk_prepare(&info->dclk);
		info->dclk.freqMaxM = CAIRNSMORE1_MAXIMUM_CLOCK / 2.5;
		info->dclk.freqM =
		info->dclk.freqMDefault = CAIRNSMORE1_DEFAULT_CLOCK / 2.5;
		cairnsmore_send_cmd(cm1->device_fd, 0, info->dclk.freqM);
		applog(LOG_WARNING, "%"PRIpreprv": Frequency set to %u MHz (range: %u-%u)",
		       cm1->proc_repr,
		       CAIRNSMORE1_DEFAULT_CLOCK, CAIRNSMORE1_MINIMUM_CLOCK, CAIRNSMORE1_MAXIMUM_CLOCK
		);
		// The dynamic-clocking firmware connects each FPGA as its own device
		if (!(info->user_set & 1)) {
			info->work_division = 1;
			if (!(info->user_set & 2))
				info->fpga_count = 1;
		}
	} else {
		applog(LOG_WARNING, "%"PRIpreprv": Frequency scaling not supported",
			cm1->proc_repr
		);
	}
	// Commands corrupt the hash state, so next scanhash is a firstrun
	state->firstrun = true;

	return true;
}

void convert_icarus_to_cairnsmore(struct cgpu_info *cm1)
{
	struct ICARUS_INFO *info = cm1->device_data;
	info->Hs = CAIRNSMORE1_HASH_TIME;
	info->fullnonce = info->Hs * (((double)0xffffffff) + 1);
	info->timing_mode = MODE_LONG;
	info->do_icarus_timing = true;
	cm1->drv = &cairnsmore_drv;
	renumber_cgpu(cm1);
	cairnsmore_init(cm1->thr[0]);
}

static struct api_data *cairnsmore_drv_extra_device_status(struct cgpu_info *cm1)
{
	struct ICARUS_INFO *info = cm1->device_data;
	struct api_data*root = NULL;

	if (info->dclk.freqM) {
		double frequency = 2.5 * info->dclk.freqM;
		root = api_add_freq(root, "Frequency", &frequency, true);
	}

	return root;
}

static bool cairnsmore_identify(struct cgpu_info *cm1)
{
	struct ICARUS_INFO *info = cm1->device_data;
	if (!info->dclk.freqM)
		return false;
	
	cairnsmore_send_cmd(cm1->device_fd, 1, 1);
	nmsleep(5000);
	cairnsmore_send_cmd(cm1->device_fd, 1, 0);
	cm1->flash_led = true;
	return true;
}

static void cairnsmore_drv_init()
{
	cairnsmore_drv = icarus_drv;
	cairnsmore_drv.dname = "cairnsmore";
	cairnsmore_drv.name = "ECM";
	cairnsmore_drv.drv_detect = cairnsmore_detect;
	cairnsmore_drv.thread_init = cairnsmore_init;
	cairnsmore_drv.identify_device = cairnsmore_identify;
	cairnsmore_drv.get_api_extra_device_status = cairnsmore_drv_extra_device_status;
}

struct device_drv cairnsmore_drv = {
	// Needed to get to cairnsmore_drv_init at all
	.drv_detect = cairnsmore_detect,
};
