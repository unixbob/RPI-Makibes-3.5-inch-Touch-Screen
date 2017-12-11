/*
 * Copyright 2012 nelisky
 * Copyright 2012-2013 Luke Dashjr
 * Copyright 2012-2013 Denis Ahrens~
 * Copyright 2012 Peter Stuge~
 *
 * This work is based upon the Java SDK provided by ztex which is
 * Copyright (C) 2009-2011 ZTEX GmbH.
 * http://www.ztex.de
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 *
 * The copyright status of some of this code is currently a bit confused. They
 * were initially released under a license (GPLv2 only) incompatible with the
 * rest of the program at the time (GPLv3 or newer), and I haven't had luck
 * getting in touch with some later contributors (denoted above with a tilde) to
 * clarify it. Since their modifications would have been a license violation,
 * I'm assuming it was just an innocent mistake on their part.
 */

#define _GNU_SOURCE

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "compat.h"
#include "dynclock.h"
#include "miner.h"
#include "fpgautils.h"
#include "libztex.h"
#include "util.h"

//* Capability index for EEPROM support.
#define CAPABILITY_EEPROM 0,0
//* Capability index for FPGA configuration support. 
#define CAPABILITY_FPGA 0,1
//* Capability index for FLASH memory support.
#define CAPABILITY_FLASH 0,2
//* Capability index for DEBUG helper support.
#define CAPABILITY_DEBUG 0,3
//* Capability index for AVR XMEGA support.
#define CAPABILITY_XMEGA 0,4
//* Capability index for AVR XMEGA support.
#define CAPABILITY_HS_FPGA 0,5
//* Capability index for AVR XMEGA support.
#define CAPABILITY_MAC_EEPROM 0,6
//* Capability index for multi FPGA support.
#define CAPABILITY_MULTI_FPGA 0,7

static int libztex_get_string_descriptor_ascii(libusb_device_handle *dev, uint8_t desc_index,
		unsigned char *data, int length)
{
	int i, cnt;
	uint16_t langid;
	unsigned char buf[260];

	/* We open code string descriptor retrieval and ASCII decoding here
	 * in order to work around that libusb_get_string_descriptor_ascii()
	 * in the FreeBSD libusb implementation hits a bug in ZTEX firmware,
	 * where the device returns more bytes than requested, causing babble,
	 * which makes FreeBSD return an error to us.
	 *
	 * Avoid the mess by doing it manually the same way as libusb-1.0.
	 */

	cnt = libusb_control_transfer(dev, LIBUSB_ENDPOINT_IN,
	    LIBUSB_REQUEST_GET_DESCRIPTOR, (LIBUSB_DT_STRING << 8) | 0,
	    0x0000, buf, sizeof(buf), 1000);
	if (cnt < 0) {
		applog(LOG_ERR, "%s: Failed to read LANGIDs: %s", __func__, bfg_strerror(cnt, BST_LIBUSB));
		return cnt;
	}

	langid = libusb_le16_to_cpu(((uint16_t *)buf)[1]);

	cnt = libusb_control_transfer(dev, LIBUSB_ENDPOINT_IN,
	    LIBUSB_REQUEST_GET_DESCRIPTOR, (LIBUSB_DT_STRING << 8) | desc_index,
	    langid, buf, sizeof(buf), 1000);
	if (cnt < 0) {
		applog(LOG_ERR, "%s: Failed to read string descriptor: %s", __func__, bfg_strerror(cnt, BST_LIBUSB));
		return cnt;
	}

	/* num chars = (all bytes except bLength and bDescriptorType) / 2 */
	for (i = 0; i <= (cnt - 2) / 2 && i < length-1; i++)
		data[i] = buf[2 + i*2];

	data[i] = 0;

	return LIBUSB_SUCCESS;
}

enum check_result
{
	CHECK_ERROR,
	CHECK_IS_NOT_ZTEX,
	CHECK_OK,
	CHECK_RESCAN,
};

static bool libztex_firmwareReset(struct libusb_device_handle *hndl, bool enable)
{
	uint8_t reset = enable ? 1 : 0;
	int cnt = libusb_control_transfer(hndl, 0x40, 0xA0, 0xE600, 0, &reset, 1, 1000);
	if (cnt < 0)
	{
		applog(LOG_ERR, "Ztex reset %d failed: %s", enable, bfg_strerror(cnt, BST_LIBUSB));
		return 1;
	}

	return 0;
}

static enum check_result libztex_checkDevice(struct libusb_device *dev)
{
	FILE *fp = NULL;
	libusb_device_handle *hndl = NULL;
	struct libusb_device_descriptor desc;
	int ret = CHECK_ERROR, err, cnt;
	size_t got_bytes, length;
	unsigned char buf[64], *fw_buf = NULL;
	unsigned int i;

	err = libusb_get_device_descriptor(dev, &desc);
	if (unlikely(err != 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to open read descriptor with error %d", err);
		return CHECK_ERROR;
	}

	if (desc.idVendor != LIBZTEX_IDVENDOR || desc.idProduct != LIBZTEX_IDPRODUCT) {
		applog(LOG_DEBUG, "Not a ZTEX device %04x:%04x", desc.idVendor, desc.idProduct);
		return CHECK_IS_NOT_ZTEX;
	}

	err = libusb_open(dev, &hndl);
	if (err != LIBUSB_SUCCESS) {
		applog(LOG_ERR, "%s: Can not open ZTEX device: %s", __func__, bfg_strerror(err, BST_LIBUSB));
		goto done;
	}

	cnt = libusb_control_transfer(hndl, 0xc0, 0x22, 0, 0, buf, 40, 500);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to read ztex descriptor with err %d", cnt);
		goto done;
	}

	if (buf[0] != 40 || buf[1] != 1 || buf[2] != 'Z' || buf[3] != 'T' || buf[4] != 'E' || buf[5] != 'X') {
		applog(LOG_ERR, "Ztex check device: Error reading ztex descriptor");
		goto done;
	}

	if (buf[6] != 10)
	{
		ret = CHECK_IS_NOT_ZTEX;
		goto done;
	}

	// 15 = 1.15y   13 = 1.15d or 1.15x
	switch(buf[7])
	{
		case 13:
			applog(LOG_ERR, "Found ztex board 1.15d or 1.15x");
			break;
		case 15:
			applog(LOG_ERR, "Found ztex board 1.15y");
			break;
		default:
			applog(LOG_ERR, "Found unknown ztex board");
			ret = CHECK_IS_NOT_ZTEX;
			goto done;
	}

	// testing for dummy firmware
	if (buf[8] != 0) {
		ret = CHECK_OK;
		goto done;
	}

	applog(LOG_ERR, "Found dummy firmware, trying to send mining firmware");

	char productString[32];

	cnt = libztex_get_string_descriptor_ascii(hndl, desc.iProduct, (unsigned char*)productString, sizeof(productString));
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to read device productString with err %d", cnt);
		return cnt;
	}

	applog(LOG_ERR, "productString: %s", productString);

	unsigned char productID2 = buf[7];
	char *firmware = NULL;

	if (strcmp("USB-FPGA Module 1.15d (default)", productString) == 0 && productID2 == 13)
	{
		firmware = "ztex_ufm1_15d4.bin";
	}
	else if (strcmp("USB-FPGA Module 1.15x (default)", productString) == 0 && productID2 == 13)
	{
		firmware = "ztex_ufm1_15d4.bin";
	}
	else if (strcmp("USB-FPGA Module 1.15y (default)", productString) == 0 && productID2 == 15)
	{
		firmware = "ztex_ufm1_15y1.bin";
	}

	if (firmware == NULL)
	{
		applog(LOG_ERR, "could not figure out which firmware to use");
		goto done;
	}

	applog(LOG_ERR, "Mining firmware filename: %s", firmware);

	fp = open_bitstream("ztex", firmware);
	if (!fp) {
		applog(LOG_ERR, "failed to open firmware file '%s'", firmware);
		goto done;
	}

	if (0 != fseek(fp, 0, SEEK_END)) {
		applog(LOG_ERR, "Ztex firmware fseek: %s", bfg_strerror(errno, BST_ERRNO));
		goto done;
	}

	length = ftell(fp);
	rewind(fp);
	fw_buf = malloc(length);
	if (!fw_buf) {
		applog(LOG_ERR, "%s: Can not allocate memory: %s", __func__, bfg_strerror(errno, BST_ERRNO));
		goto done;
	}

	got_bytes = fread(fw_buf, 1, length, fp);
	fclose(fp);
	fp = NULL;

	if (got_bytes < length) {
		applog(LOG_ERR, "%s: Incomplete firmware read: %"PRIu64"/%"PRIu64,
		       __func__, (uint64_t)got_bytes, (uint64_t)length);
		goto done;
	}

	// in buf[] is still the identifier of the dummy firmware
	// use it to compare it with the new firmware
	char *rv = memmem(fw_buf, got_bytes, buf, 8);
	if (rv == NULL)
	{
		applog(LOG_ERR, "%s: found firmware is not ZTEX", __func__);
		goto done;
	}

	// check for dummy firmware
	if (rv[8] == 0)
	{
		applog(LOG_ERR, "%s: found a ZTEX dummy firmware", __func__);
		goto done;
	}

	if (libztex_firmwareReset(hndl, true))
		goto done;

	for (i = 0; i < length; i+= 256) {
		// firmware wants data in small chunks like 256 bytes
		int numbytes = (length - i) < 256 ? (length - i) : 256;
		int k = libusb_control_transfer(hndl, 0x40, 0xA0, i, 0, fw_buf + i, numbytes, 1000);
		if (k < numbytes)
		{
			applog(LOG_ERR, "Ztex device: Failed to write firmware at %d with: %s", i, bfg_strerror(k, BST_LIBUSB));
			goto done;
		}
	}

	if (libztex_firmwareReset(hndl, false))
		goto done;

	applog(LOG_ERR, "Ztex device: succesfully wrote firmware");
	ret = CHECK_RESCAN;

done:
	free(fw_buf);
	if (fp)
		fclose(fp);
	if (hndl)
		libusb_close(hndl);
	return ret;
}

static bool libztex_checkCapability(struct libztex_device *ztex, int i, int j)
{
	if (!((i >= 0) && (i <= 5) && (j >= 0) && (j < 8) &&
	     (((ztex->interfaceCapabilities[i] & 255) & (1 << j)) != 0))) {
		applog(LOG_ERR, "%s: capability missing: %d %d", ztex->repr, i, j);
		return false;
	}
	return true;
}

static char libztex_detectBitstreamBitOrder(const unsigned char *buf, int size)
{
	int i;

	for (i = 0; i < size - 4; i++) {
		if (((buf[i] & 255) == 0xaa) && ((buf[i + 1] & 255) == 0x99) && ((buf[i + 2] & 255) == 0x55) && ((buf[i + 3] & 255) == 0x66))
			return 1;
		if (((buf[i] & 255) == 0x55) && ((buf[i + 1] & 255) == 0x99) && ((buf[i + 2] & 255) == 0xaa) && ((buf[i + 3] & 255) == 0x66))
			return 0;
	} 
	applog(LOG_WARNING, "Unable to determine bitstream bit order: no signature found");
	return 0;
}

static void libztex_swapBits(unsigned char *buf, int size)
{
	unsigned char c;
	int i;

	for (i = 0; i < size; i++) {
		c = buf[i];
		buf[i] = ((c & 128) >> 7) |
		         ((c & 64) >> 5) |
		         ((c & 32) >> 3) |
		         ((c & 16) >> 1) |
		         ((c & 8) << 1) |
		         ((c & 4) << 3) |
		         ((c & 2) << 5) |
		         ((c & 1) << 7);
	}
}

static int libztex_getFpgaState(struct libztex_device *ztex, struct libztex_fpgastate *state)
{
	unsigned char buf[9];
	int cnt;

	if (!libztex_checkCapability(ztex, CAPABILITY_FPGA))
		return -1;
	cnt = libusb_control_transfer(ztex->hndl, 0xc0, 0x30, 0, 0, buf, 9, 1000);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "%s: Failed getFpgaState with err %d", ztex->repr, cnt);
		return cnt;
	}
	state->fpgaConfigured = (buf[0] == 0);
	state->fpgaChecksum = buf[1] & 0xff;
	state->fpgaBytes = ((buf[5] & 0xff) << 24) | ((buf[4] & 0xff) << 16) | ((buf[3] & 0xff) << 8) | (buf[2] & 0xff);
	state->fpgaInitB = buf[6] & 0xff;
	state->fpgaFlashResult = buf[7];
	state->fpgaFlashBitSwap = (buf[8] != 0);
	return 0;
}

static int libztex_configureFpgaHS(struct libztex_device *ztex, const char* firmware, bool force, char bs, const char *repr)
{
	struct libztex_fpgastate state;
	const int transactionBytes = 65536;
	unsigned char buf[transactionBytes], settings[2];
	int tries, cnt, err;
	FILE *fp;

	if (!libztex_checkCapability(ztex, CAPABILITY_HS_FPGA))
		return -1;
	libztex_getFpgaState(ztex, &state);
	if (!force && state.fpgaConfigured) {
		applog(LOG_INFO, "Bitstream already configured");
		return 0;
	}
	cnt = libusb_control_transfer(ztex->hndl, 0xc0, 0x33, 0, 0, settings, 2, 1000);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "%s: Failed getHSFpgaSettings with err %d", ztex->repr, cnt);
		return cnt;
	}

	err = libusb_claim_interface(ztex->hndl, settings[1]);
	if (err != LIBUSB_SUCCESS) {
		applog(LOG_ERR, "%s: failed to claim interface for hs transfer", ztex->repr);
		return -4;
	}

	for (tries = 3; tries > 0; tries--) {
		fp = open_bitstream("ztex", firmware);
		if (!fp) {
			applog(LOG_ERR, "%"PRIpreprv": failed to read bitstream '%s'", repr, firmware);
			libusb_release_interface(ztex->hndl, settings[1]);
			return -2;
		}

		libusb_control_transfer(ztex->hndl, 0x40, 0x34, 0, 0, NULL, 0, 1000);
		// 0x34 - initHSFPGAConfiguration

		do
		{
			int length = fread(buf,1,transactionBytes,fp);

			if (bs != 0 && bs != 1)
				bs = libztex_detectBitstreamBitOrder(buf, length);
			if (bs == 1)
				libztex_swapBits(buf, length);

			err = libusb_bulk_transfer(ztex->hndl, settings[0], buf, length, &cnt, 1000);
			if (cnt != length)
				applog(LOG_ERR, "%s: cnt != length", ztex->repr);
			if (err != 0)
				applog(LOG_ERR, "%s: Failed send hs fpga data", ztex->repr);
		}
		while (!feof(fp));

		// While 1.15y can finish immediately, at least 1.15x needs some delay
		// (200ms might be enough, but 500ms is safer)
		if (ztex->productId[1] != 15)
			usleep(500);

		libusb_control_transfer(ztex->hndl, 0x40, 0x35, 0, 0, NULL, 0, 1000);
		// 0x35 - finishHSFPGAConfiguration
		if (cnt >= 0)
			tries = 0;

		fclose(fp);

		libztex_getFpgaState(ztex, &state);
		if (!state.fpgaConfigured) {
			applog(LOG_ERR, "%"PRIpreprv": HS FPGA configuration failed: DONE pin does not go high", repr);
			libusb_release_interface(ztex->hndl, settings[1]);
			return -3;
		}
	}

	libusb_release_interface(ztex->hndl, settings[1]);

	nmsleep(200);
	applog(LOG_INFO, "%"PRIpreprv": HS FPGA configuration done", repr);
	return 0;
}

static int libztex_configureFpgaLS(struct libztex_device *ztex, const char* firmware, bool force, char bs, const char *repr)
{
	struct libztex_fpgastate state;
	const int transactionBytes = 2048;
	unsigned char buf[transactionBytes];
	int tries, cnt;
	FILE *fp;

	if (!libztex_checkCapability(ztex, CAPABILITY_FPGA))
		return -1;

	libztex_getFpgaState(ztex, &state);
	if (!force && state.fpgaConfigured) {
		applog(LOG_DEBUG, "Bitstream already configured");
		return 0;
	}

	for (tries = 10; tries > 0; tries--) {
		fp = open_bitstream("ztex", firmware);
		if (!fp) {
			applog(LOG_ERR, "%"PRIpreprv": failed to read bitstream '%s'", repr, firmware);
			return -2;
		}

		//* Reset fpga
		cnt = libztex_resetFpga(ztex);
		if (unlikely(cnt < 0)) {
			applog(LOG_ERR, "%s: Failed reset fpga with err %d", ztex->repr, cnt);
			continue;
		}

		do
		{
			int length = fread(buf, 1, transactionBytes, fp);

			if (bs != 0 && bs != 1)
				bs = libztex_detectBitstreamBitOrder(buf, length);
			if (bs == 1)
				libztex_swapBits(buf, length);
			cnt = libusb_control_transfer(ztex->hndl, 0x40, 0x32, 0, 0, buf, length, 5000);
			if (cnt != length)
			{
				applog(LOG_ERR, "%s: Failed send ls fpga data", ztex->repr);
				break;
			}
		}
		while (!feof(fp));

		if (cnt > 0)
			tries = 0;

		fclose(fp);
	}

	libztex_getFpgaState(ztex, &state);
	if (!state.fpgaConfigured) {
		applog(LOG_ERR, "%"PRIpreprv": LS FPGA configuration failed: DONE pin does not go high", repr);
		return -3;
	}

	nmsleep(200);
	applog(LOG_INFO, "%"PRIpreprv": FPGA configuration done", repr);
	return 0;
}

int libztex_configureFpga(struct libztex_device *ztex, const char *repr)
{
	char buf[256];
	int rv;

	strcpy(buf, ztex->bitFileName);
	strcat(buf, ".bit");
	rv = libztex_configureFpgaHS(ztex, buf, true, 2, repr);
	if (rv != 0)
		rv = libztex_configureFpgaLS(ztex, buf, true, 2, repr);
	return rv;
}

int libztex_numberOfFpgas(struct libztex_device *ztex)
{
	int cnt;
	unsigned char buf[3];

	if (ztex->numberOfFpgas < 0) {
		if (libztex_checkCapability(ztex, CAPABILITY_MULTI_FPGA)) {
			cnt = libusb_control_transfer(ztex->hndl, 0xc0, 0x50, 0, 0, buf, 3, 1000);
			if (unlikely(cnt < 0)) {
				applog(LOG_ERR, "%s: Failed getMultiFpgaInfo with err %d", ztex->repr, cnt);
				return cnt;
			}
			ztex->numberOfFpgas = buf[0] + 1;
			ztex->selectedFpga = -1;//buf[1];
			ztex->parallelConfigSupport = (buf[2] == 1);
		} else {
			ztex->numberOfFpgas = 1;
			ztex->selectedFpga = -1;//0;
			ztex->parallelConfigSupport = false;
		}
	}
	return ztex->numberOfFpgas;
}

int libztex_selectFpga(struct libztex_device *ztex, int16_t number)
{
	int cnt, fpgacnt = libztex_numberOfFpgas(ztex->root);

	if (number < 0 || number >= fpgacnt) {
		applog(LOG_WARNING, "%s: Trying to select wrong fpga (%d in %d)", ztex->repr, number, fpgacnt);
		return 1;
	}
	if (ztex->root->selectedFpga != number && libztex_checkCapability(ztex->root, CAPABILITY_MULTI_FPGA)) {
		cnt = libusb_control_transfer(ztex->root->hndl, 0x40, 0x51, (uint16_t)number, 0, NULL, 0, 500);
		if (unlikely(cnt < 0)) {
			applog(LOG_ERR, "Ztex check device: Failed to set fpga with err %d", cnt);
			ztex->root->selectedFpga = -1;
			return cnt;
		}
		ztex->root->selectedFpga = number;
	}
	return 0;
}

int libztex_setFreq(struct libztex_device *ztex, uint16_t freq, const char *repr)
{
	int cnt;
	uint16_t oldfreq = ztex->dclk.freqM;

	if (freq > ztex->dclk.freqMaxM)
		freq = ztex->dclk.freqMaxM;

	cnt = libusb_control_transfer(ztex->hndl, 0x40, 0x83, freq, 0, NULL, 0, 500);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to set frequency with err %d", cnt);
		return cnt;
	}
	ztex->dclk.freqM = freq;
	if (oldfreq > ztex->dclk.freqMaxM)
		applog(LOG_WARNING, "%"PRIpreprv": Frequency set to %u MHz (range: %u-%u)",
		       repr,
		       (unsigned)(ztex->freqM1 * (ztex->dclk.freqM + 1)),
		       (unsigned)ztex->freqM1,
		       (unsigned)(ztex->freqM1 * (ztex->dclk.freqMaxM + 1))
		);
	else
		dclk_msg_freqchange(repr,
		                    ztex->freqM1 * (oldfreq + 1),
		                    ztex->freqM1 * (ztex->dclk.freqM + 1),
		                    NULL);

	return 0;
}

int libztex_resetFpga(struct libztex_device *ztex)
{
	return libusb_control_transfer(ztex->hndl, 0x40, 0x31, 0, 0, NULL, 0, 1000);
}

int libztex_suspend(struct libztex_device *ztex)
{
	if (ztex->suspendSupported) {
		return libusb_control_transfer(ztex->hndl, 0x40, 0x84, 0, 0, NULL, 0, 1000);
	} else {
		return 0;
	}
}

int libztex_prepare_device(struct libusb_device *dev, struct libztex_device** ztex)
{
	struct libztex_device *newdev = *ztex;
	int i, cnt, err;
	unsigned char buf[64];

	dclk_prepare(&newdev->dclk);
	err = libusb_open(dev, &newdev->hndl);
	if (err != LIBUSB_SUCCESS) {
		applog(LOG_ERR, "%s: Can not open ZTEX device: %s", __func__, bfg_strerror(err, BST_LIBUSB));
		return CHECK_ERROR;
	}

	err = libusb_get_device_descriptor(dev, &newdev->descriptor);
	if (unlikely(err != 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to open read descriptor with error %d", err);
		return CHECK_ERROR;
	}

	cnt = libztex_get_string_descriptor_ascii(newdev->hndl, newdev->descriptor.iSerialNumber, newdev->snString, sizeof(newdev->snString));
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to read device snString with err %d", cnt);
		return cnt;
	}
	
	cnt = libztex_get_string_descriptor_ascii(newdev->hndl, newdev->descriptor.iProduct, buf, sizeof(buf));
	if (unlikely(cnt < 0))
		applog(LOG_WARNING, "Ztex check device: Failed to read device product with err %d", cnt);
	else
		newdev->dev_product = buf[0] ? strdup((void*)buf) : NULL;
	
	cnt = libztex_get_string_descriptor_ascii(newdev->hndl, newdev->descriptor.iManufacturer, buf, sizeof(buf));
	if (unlikely(cnt < 0))
		applog(LOG_WARNING, "Ztex check device: Failed to read device manufacturer with err %d", cnt);
	else
		newdev->dev_manufacturer = buf[0] ? strdup((void*)buf) : NULL;

	cnt = libusb_control_transfer(newdev->hndl, 0xc0, 0x22, 0, 0, buf, 40, 500);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to read ztex descriptor with err %d", cnt);
		return cnt;
	}

	if (buf[0] != 40 || buf[1] != 1 || buf[2] != 'Z' || buf[3] != 'T' || buf[4] != 'E' || buf[5] != 'X') {
		applog(LOG_ERR, "Ztex check device: Error reading ztex descriptor");
		return 2;
	}

	newdev->productId[0] = buf[6];
	newdev->productId[1] = buf[7];
	newdev->productId[2] = buf[8];
	newdev->productId[3] = buf[9];
	newdev->fwVersion = buf[10];
	newdev->interfaceVersion = buf[11];
	newdev->interfaceCapabilities[0] = buf[12];
	newdev->interfaceCapabilities[1] = buf[13];
	newdev->interfaceCapabilities[2] = buf[14];
	newdev->interfaceCapabilities[3] = buf[15];
	newdev->interfaceCapabilities[4] = buf[16];
	newdev->interfaceCapabilities[5] = buf[17];
	newdev->moduleReserved[0] = buf[18];
	newdev->moduleReserved[1] = buf[19];
	newdev->moduleReserved[2] = buf[20];
	newdev->moduleReserved[3] = buf[21];
	newdev->moduleReserved[4] = buf[22];
	newdev->moduleReserved[5] = buf[23];
	newdev->moduleReserved[6] = buf[24];
	newdev->moduleReserved[7] = buf[25];
	newdev->moduleReserved[8] = buf[26];
	newdev->moduleReserved[9] = buf[27];
	newdev->moduleReserved[10] = buf[28];
	newdev->moduleReserved[11] = buf[29];

	cnt = libusb_control_transfer(newdev->hndl, 0xc0, 0x82, 0, 0, buf, 64, 500);
	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "Ztex check device: Failed to read ztex descriptor with err %d", cnt);
		return cnt;
	}

	if (unlikely(buf[0] != 5)) {
		if (unlikely(buf[0] != 2 && buf[0] != 4)) {
			applog(LOG_ERR, "Invalid BTCMiner descriptor version. Firmware must be updated (%d).", buf[0]);
			return 3;
		}
		applog(LOG_WARNING, "Firmware out of date (%d).", buf[0]);
	}

	i = buf[0] > 4? 11: (buf[0] > 2? 10: 8);

	while (cnt < 64 && buf[cnt] != 0)
		cnt++;
	if (cnt < i + 1) {
		applog(LOG_ERR, "Invalid bitstream file name .");
		return 4;
	}

	newdev->bitFileName = malloc(sizeof(char) * (cnt + 1));
	memcpy(newdev->bitFileName, &buf[i], cnt);
	newdev->bitFileName[cnt] = 0;	

	newdev->numNonces = buf[1] + 1;
	newdev->offsNonces = ((buf[2] & 255) | ((buf[3] & 255) << 8)) - 10000;
	newdev->freqM1 = ((buf[4] & 255) | ((buf[5] & 255) << 8) ) * 0.01;
	newdev->dclk.freqMaxM = (buf[7] & 255);
	newdev->dclk.freqM = (buf[6] & 255);
	newdev->dclk.freqMDefault = newdev->dclk.freqM;
	newdev->suspendSupported = (buf[0] == 5);
	newdev->hashesPerClock = buf[0] > 2? (((buf[8] & 255) | ((buf[9] & 255) << 8)) + 1) / 128.0: 1.0;
	newdev->extraSolutions = buf[0] > 4? buf[10]: 0;

	applog(LOG_DEBUG, "PID: %d numNonces: %d offsNonces: %d freqM1: %f freqMaxM: %d freqM: %d suspendSupported: %s hashesPerClock: %f extraSolutions: %d",
	                 buf[0], newdev->numNonces, newdev->offsNonces, newdev->freqM1, newdev->dclk.freqMaxM, newdev->dclk.freqM, newdev->suspendSupported ? "T": "F",
	                 newdev->hashesPerClock, newdev->extraSolutions);

	if (buf[0] < 4) {
		if (strncmp(newdev->bitFileName, "ztex_ufm1_15b", 13) != 0)
			newdev->hashesPerClock = 0.5;
		applog(LOG_WARNING, "HASHES_PER_CLOCK not defined, assuming %0.2f", newdev->hashesPerClock);
	}

	newdev->usbbus = libusb_get_bus_number(dev);
	newdev->usbaddress = libusb_get_device_address(dev);
	sprintf(newdev->repr, "ZTEX %s-1", newdev->snString);
	return 0;
}

void libztex_destroy_device(struct libztex_device* ztex)
{
	if (ztex->hndl != NULL) {
		libusb_close(ztex->hndl);
		ztex->hndl = NULL;
	}
	if (ztex->bitFileName != NULL) {
		free(ztex->bitFileName);
		ztex->bitFileName = NULL;
	}
	free(ztex);
}

int libztex_scanDevices(struct libztex_dev_list*** devs_p)
{
	int usbdevices[LIBZTEX_MAX_DESCRIPTORS];
	struct libztex_dev_list **devs = NULL;
	struct libztex_device *ztex = NULL;
	int found, max_found = 0, pos = 0, err, rescan, ret = 0;
	libusb_device **list = NULL;
	ssize_t cnt, i;
	int skipped = 0;

	do {
		cnt = libusb_get_device_list(NULL, &list);
		if (unlikely(cnt < 0)) {
			applog(LOG_ERR, "Ztex scan devices: Failed to list usb devices with err %"PRId64, (int64_t)cnt);
			goto done;
		}

		for (found = rescan = i = 0; i < cnt; i++) {
			if (bfg_claim_libusb(NULL, false, list[i]))
			{
				++skipped;
				continue;
			}
			
			err = libztex_checkDevice(list[i]);
			switch (err) {
			case CHECK_ERROR:
				applog(LOG_ERR, "Ztex: Can not check device: %s", bfg_strerror(err, BST_LIBUSB));
				continue;
			case CHECK_IS_NOT_ZTEX:
				continue;
			case CHECK_OK:
				// Got one!
				usbdevices[found++] = i;
				break;
			case CHECK_RESCAN:
				rescan = 1;
				found++;
				break;
			}
		}

		if (found < max_found)
			rescan = 1;
		else if (found > max_found)
			max_found = found;

		if (rescan)
			libusb_free_device_list(list, 1);
	} while (rescan);

	if (0 == found)
		goto done;

	devs = malloc(sizeof(struct libztex_dev_list *) * found);
	if (devs == NULL) {
		applog(LOG_ERR, "Ztex scan devices: Failed to allocate memory");
		goto done;
	}

	for (i = 0; i < found; i++) {
		if (!ztex) {
			ztex = malloc(sizeof(*ztex));
			if (!ztex) {
				applog(LOG_ERR, "%s: Can not allocate memory for device struct: %s", __func__, bfg_strerror(errno, BST_ERRNO));
				goto done;
			}
		}

		ztex->bitFileName = NULL;
		ztex->numberOfFpgas = -1;

		err = libztex_prepare_device(list[usbdevices[i]], &ztex);
		if (unlikely(err != 0)) {
			applog(LOG_ERR, "prepare device: %d", err);
			libztex_destroy_device(ztex);
			ztex = NULL;
			continue;
		}

		devs[pos] = malloc(sizeof(struct libztex_dev_list));
		if (NULL == devs[pos]) {
			applog(LOG_ERR, "%s: Can not allocate memory for device: %s", __func__, bfg_strerror(errno, BST_ERRNO));
			libztex_destroy_device(ztex);
			ztex = NULL;
			continue;
		}

		devs[pos]->dev = ztex;
		ztex = NULL;
		devs[pos]->next = NULL;
		if (pos > 0)
			devs[pos - 1]->next = devs[pos];
		pos++;
	}

	ret = pos;

done:
	if (ret > 0)
		*devs_p = devs;
	else if (devs)
		free(devs);
	if (list)
		libusb_free_device_list(list, 1);
	
	if (skipped)
		applog(LOG_DEBUG, "%s: Skipping probe of %d claimed devices", __func__, skipped);
	
	return ret;
}

int libztex_sendHashData(struct libztex_device *ztex, unsigned char *sendbuf)
{
	int cnt = 0, ret, len;

	if (ztex == NULL || ztex->hndl == NULL)
		return 0;
	ret = 44; len = 0;
	while (ret > 0) {
		cnt = libusb_control_transfer(ztex->hndl, 0x40, 0x80, 0, 0, sendbuf + len, ret, 1000);
		if (cnt >= 0) {
			ret -= cnt;
			len += cnt;
		} else
			break;
	}
	if (unlikely(cnt < 0))
		applog(LOG_ERR, "%s: Failed sendHashData with err %d", ztex->repr, cnt);

	return cnt;
}

int libztex_readHashData(struct libztex_device *ztex, struct libztex_hash_data nonces[])
{
	int bufsize = 12 + ztex->extraSolutions * 4;
	int cnt = 0, i, j, ret, len;
	unsigned char *rbuf;

	if (ztex->hndl == NULL)
		return 0;

	rbuf = malloc(sizeof(unsigned char) * (ztex->numNonces * bufsize));
	if (rbuf == NULL) {
		applog(LOG_ERR, "%s: Failed to allocate memory for reading nonces", ztex->repr);
		return 0;
	}
	ret = bufsize * ztex->numNonces; len = 0;
	while (ret > 0) {
		cnt = libusb_control_transfer(ztex->hndl, 0xc0, 0x81, 0, 0, rbuf + len, ret, 1000);
		if (cnt >= 0) {
			ret -= cnt;
			len += cnt;
		} else
			break;
	}

	if (unlikely(cnt < 0)) {
		applog(LOG_ERR, "%s: Failed readHashData with err %d", ztex->repr, cnt);
		free(rbuf);
		return cnt;
	}

	for (i=0; i<ztex->numNonces; i++) {
		uint32_t *nonce_data = (void*)&rbuf[i * bufsize];
		nonces[i].goldenNonce[0] = nonce_data[0] - ztex->offsNonces;
		//applog(LOG_DEBUG, "W %d:0 %0.8x", i, nonces[i].goldenNonce[0]);

		nonces[i].nonce = le32toh(nonce_data[1]) - ztex->offsNonces;
		nonces[i].hash7 = le32toh(nonce_data[2]);

		for (j = 1; j <= ztex->extraSolutions; ++j)
		{
			nonces[i].goldenNonce[j] = le32toh(nonce_data[2 + j]) - ztex->offsNonces;
			//applog(LOG_DEBUG, "W %d:%d %0.8x", i, j, nonces[i].goldenNonce[j]);
		}
	}

	free(rbuf);
	return cnt;
}

void libztex_freeDevList(struct libztex_dev_list **devs)
{
	bool done = false;
	ssize_t cnt = 0;

	while (!done) {
		if (devs[cnt]->next == NULL)
			done = true;
		free(devs[cnt++]);
	}
	free(devs);
}

