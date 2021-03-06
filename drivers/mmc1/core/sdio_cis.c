/*
 * linux/drivers/mmc/core/sdio_cis.c
 *
 * Author:	Nicolas Pitre
 * Created:	June 11, 2007
 * Copyright:	MontaVista Software Inc.
 *
 * Copyright 2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/kernel.h>

#include <linux/mmc1/host.h>
#include <linux/mmc1/card.h>
#include <linux/mmc1/sdio.h>
#include <linux/mmc1/sdio_func.h>

#include "sdio_cis.h"
#include "sdio_ops.h"

#if 0
#define DBG(x...)	printk(KERN_ALERT x)
#else
#define DBG(x...)	do { } while (0)
#endif

static int cistpl_vers_1(struct mmc_card *card, struct sdio_func *func,
			 const unsigned char *buf, unsigned size)
{
	unsigned i, nr_strings;
	char **buffer, *string;

	/* Find all null-terminated (including zero length) strings in
	   the TPLLV1_INFO field. Trailing garbage is ignored. */
	buf += 2;
	size -= 2;

	nr_strings = 0;
	for (i = 0; i < size; i++) {
		if (buf[i] == 0xff)
			break;
		if (buf[i] == 0)
			nr_strings++;
	}
	if (nr_strings == 0)
		return 0;

	size = i;

	buffer = kzalloc(sizeof(char*) * nr_strings + size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	string = (char*)(buffer + nr_strings);

	for (i = 0; i < nr_strings; i++) {
		buffer[i] = string;
		strcpy(string, buf);
		string += strlen(string) + 1;
		buf += strlen(buf) + 1;
	}

	if (func) {
		func->num_info = nr_strings;
		func->info = (const char**)buffer;
	} else {
		card->num_info = nr_strings;
		card->info = (const char**)buffer;
	}

	DBG("[%s] e3\n",__func__);
	return 0;
}

static int cistpl_manfid(struct mmc_card *card, struct sdio_func *func,
			 const unsigned char *buf, unsigned size)
{
	unsigned int vendor, device;
	DBG("[%s] s\n",__func__);
	/* TPLMID_MANF */
	vendor = buf[0] | (buf[1] << 8);

	/* TPLMID_CARD */
	device = buf[2] | (buf[3] << 8);

	if (func) {
		func->vendor = vendor;
		func->device = device;
	} else {
		card->cis.vendor = vendor;
		card->cis.device = device;
	}
	DBG("[%s] e\n",__func__);
	return 0;
}

static const unsigned char speed_val[16] =
	{ 0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80 };
static const unsigned int speed_unit[8] =
	{ 10000, 100000, 1000000, 10000000, 0, 0, 0, 0 };

/* FUNCE tuples with these types get passed to SDIO drivers */
static const unsigned char funce_type_whitelist[] = {
	4 /* CISTPL_FUNCE_LAN_NODE_ID used in Broadcom cards */
};

static int cistpl_funce_whitelisted(unsigned char type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(funce_type_whitelist); i++) {
		if (funce_type_whitelist[i] == type)
			return 1;
	}
	return 0;
}

static int cistpl_funce_common(struct mmc_card *card,
			       const unsigned char *buf, unsigned size)
{
	DBG("[%s] s\n",__func__);
	
	if (size < 0x04 || buf[0] != 0) {
		DBG("[%s] e1\n",__func__);
		return -EINVAL;
	}

	/* TPLFE_FN0_BLK_SIZE */
	card->cis.blksize = buf[1] | (buf[2] << 8);

	/* TPLFE_MAX_TRAN_SPEED */
	card->cis.max_dtr = speed_val[(buf[3] >> 3) & 15] *
			    speed_unit[buf[3] & 7];

	DBG("[%s] e2\n",__func__);
	return 0;
}

static int cistpl_funce_func(struct sdio_func *func,
			     const unsigned char *buf, unsigned size)
{
	unsigned vsn;
	unsigned min_size;

	DBG("[%s] s\n",__func__);
	/* let SDIO drivers take care of whitelisted FUNCE tuples */
	if (cistpl_funce_whitelisted(buf[0]))
		return -EILSEQ;

	vsn = func->card->cccr.sdio_vsn;
	min_size = (vsn == SDIO_SDIO_REV_1_00) ? 28 : 42;

	if (size < min_size || buf[0] != 1) {
		DBG("[%s] e1\n",__func__);
		return -EINVAL;
	}

	/* TPLFE_MAX_BLK_SIZE */
	func->max_blksize = buf[12] | (buf[13] << 8);

	/* TPLFE_ENABLE_TIMEOUT_VAL, present in ver 1.1 and above */
	if (vsn > SDIO_SDIO_REV_1_00)
		func->enable_timeout = (buf[28] | (buf[29] << 8)) * 10;
	else
		func->enable_timeout = jiffies_to_msecs(HZ);

	DBG("[%s] e2\n",__func__);
	return 0;
}

static int cistpl_funce(struct mmc_card *card, struct sdio_func *func,
			const unsigned char *buf, unsigned size)
{
	int ret;
	DBG("[%s] s\n",__func__);
	
	/*
	 * There should be two versions of the CISTPL_FUNCE tuple,
	 * one for the common CIS (function 0) and a version used by
	 * the individual function's CIS (1-7). Yet, the later has a
	 * different length depending on the SDIO spec version.
	 */
	if (func)
		ret = cistpl_funce_func(func, buf, size);
	else
		ret = cistpl_funce_common(card, buf, size);

	if (ret && ret != -EILSEQ) {
		printk(KERN_ERR "%s: bad CISTPL_FUNCE size %u "
		       "type %u\n", mmc1_hostname(card->host), size, buf[0]);
	}

	DBG("[%s] e2\n",__func__);
	return ret;
}

typedef int (tpl_parse_t)(struct mmc_card *, struct sdio_func *,
			   const unsigned char *, unsigned);

struct cis_tpl {
	unsigned char code;
	unsigned char min_size;
	tpl_parse_t *parse;
};

static const struct cis_tpl cis_tpl_list[] = {
	{	0x15,	3,	cistpl_vers_1		},
	{	0x20,	4,	cistpl_manfid		},
	{	0x21,	2,	/* cistpl_funcid */	},
	{	0x22,	0,	cistpl_funce		},
};

static int sdio1_read_cis(struct mmc_card *card, struct sdio_func *func)
{
	int ret;
	struct sdio_func_tuple *this, **prev;
	unsigned i, ptr = 0;
	DBG("[%s] s\n",__func__);

	/*
	 * Note that this works for the common CIS (function number 0) as
	 * well as a function's CIS * since SDIO_CCCR_CIS and SDIO_FBR_CIS
	 * have the same offset.
	 */
	for (i = 0; i < 3; i++) {
		unsigned char x, fn;

		if (func)
			fn = func->num;
		else
			fn = 0;

		ret = mmc1_io_rw_direct(card, 0, 0,
			SDIO_FBR_BASE(fn) + SDIO_FBR_CIS + i, 0, &x);
		if (ret) {
			DBG("[%s] e1\n",__func__);
			return ret;
		}
		ptr |= x << (i * 8);
	}

	if (func)
		prev = &func->tuples;
	else
		prev = &card->tuples;

	BUG_ON(*prev);

	do {
		unsigned char tpl_code, tpl_link;

		ret = mmc1_io_rw_direct(card, 0, 0, ptr++, 0, &tpl_code);
		if (ret)
			break;

		/* 0xff means we're done */
		if (tpl_code == 0xff)
			break;

		/* null entries have no link field or data */
		if (tpl_code == 0x00)
			continue;

		ret = mmc1_io_rw_direct(card, 0, 0, ptr++, 0, &tpl_link);
		if (ret)
			break;

		/* a size of 0xff also means we're done */
		if (tpl_link == 0xff)
			break;

		this = kmalloc(sizeof(*this) + tpl_link, GFP_KERNEL);
		if (!this)
			return -ENOMEM;

		for (i = 0; i < tpl_link; i++) {
			ret = mmc1_io_rw_direct(card, 0, 0,
					       ptr + i, 0, &this->data[i]);
			if (ret)
				break;
		}
		if (ret) {
			kfree(this);
			break;
		}

		for (i = 0; i < ARRAY_SIZE(cis_tpl_list); i++)
			if (cis_tpl_list[i].code == tpl_code)
				break;
		if (i < ARRAY_SIZE(cis_tpl_list)) {
			const struct cis_tpl *tpl = cis_tpl_list + i;
			if (tpl_link < tpl->min_size) {
				printk(KERN_ERR
				       "%s: bad CIS tuple 0x%02x"
				       " (length = %u, expected >= %u)\n",
				       mmc1_hostname(card->host),
				       tpl_code, tpl_link, tpl->min_size);
				ret = -EINVAL;
			} else if (tpl->parse) {
				ret = tpl->parse(card, func,
						 this->data, tpl_link);
			}
			/*
			 * We don't need the tuple anymore if it was
			 * successfully parsed by the SDIO core or if it is
			 * not going to be parsed by SDIO drivers.
			 */
			if (!ret || ret != -EILSEQ)
				kfree(this);
		} else {
			/* unknown tuple */
			ret = -EILSEQ;
		}

		if (ret == -EILSEQ) {
			/* this tuple is unknown to the core or whitelisted */
			this->next = NULL;
			this->code = tpl_code;
			this->size = tpl_link;
			*prev = this;
			prev = &this->next;
			printk(KERN_DEBUG
			       "%s: queuing CIS tuple 0x%02x length %u\n",
			       mmc1_hostname(card->host), tpl_code, tpl_link);
			/* keep on analyzing tuples */
			ret = 0;
		}

		ptr += tpl_link;
	} while (!ret);

	/*
	 * Link in all unknown tuples found in the common CIS so that
	 * drivers don't have to go digging in two places.
	 */
	if (func)
		*prev = card->tuples;

	DBG("[%s] e3\n",__func__);
	return ret;
}

int sdio1_read_common_cis(struct mmc_card *card)
{
	int ret;
	DBG("[%s] s\n",__func__);
	ret = sdio1_read_cis(card, NULL);
	DBG("[%s] e\n",__func__);
	
	return ret;
}

void sdio1_free_common_cis(struct mmc_card *card)
{
	struct sdio_func_tuple *tuple, *victim;
	
	tuple = card->tuples;

	while (tuple) {
		victim = tuple;
		tuple = tuple->next;
		kfree(victim);
	}

	card->tuples = NULL;
	DBG("[%s] e\n",__func__);
}

int sdio1_read_func_cis(struct sdio_func *func)
{
	int ret;
	DBG("[%s] s\n",__func__);
	
	ret = sdio1_read_cis(func->card, func);
	if (ret) {
		DBG("[%s] e1\n",__func__);
		return ret;
	}

	/*
	 * Since we've linked to tuples in the card structure,
	 * we must make sure we have a reference to it.
	 */
	get_device(&func->card->dev);

	/*
	 * Vendor/device id is optional for function CIS, so
	 * copy it from the card structure as needed.
	 */
	if (func->vendor == 0) {
		func->vendor = func->card->cis.vendor;
		func->device = func->card->cis.device;
	}

	DBG("[%s] e2\n",__func__);
	return 0;
}

void sdio1_free_func_cis(struct sdio_func *func)
{
	struct sdio_func_tuple *tuple, *victim;
	DBG("[%s] s\n",__func__);
	
	tuple = func->tuples;

	while (tuple && tuple != func->card->tuples) {
		victim = tuple;
		tuple = tuple->next;
		kfree(victim);
	}

	func->tuples = NULL;

	/*
	 * We have now removed the link to the tuples in the
	 * card structure, so remove the reference.
	 */
	put_device(&func->card->dev);
	DBG("[%s] e\n",__func__);
}

