/*
 * usb_ctrl_std.c
 *
 * Copyright (C) 2019-2021  Sylvain Munaut <tnt@246tNt.com>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <no2usb/usb_hw.h>
#include <no2usb/usb_priv.h>

#include "console.h"


	/* Control Request implementation */

static bool
_get_status_dev(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	xfer->data[0] = 0x00;	/* No remote wakeup, bus-powered */
	xfer->data[1] = 0x00;
	xfer->len = 2;
	return true;
}

static bool
_get_status_intf(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	/* Check interface exits */
	if (usb_desc_find_intf(NULL, req->wIndex, 0, NULL))
		return false;

	/* Nothing to return really */
	xfer->data[0] = 0x00;
	xfer->data[1] = 0x00;
	xfer->len = 2;
	return true;
}

static bool
_get_status_ep(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	uint8_t ep = req->wIndex;

	if (!usb_ep_is_configured(ep))
		return false;

	xfer->data[0] = usb_ep_is_halted(ep) ? 0x01 : 0x00;
	xfer->data[1] = 0x00;
	xfer->len = 2;
	return true;
}

static bool
_clear_feature_dev(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	/* No support for any device feature */
	return false;
}

static bool
_clear_feature_intf(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	/* No support for any interface feature */
	return false;
}

static bool
_clear_feature_ep(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	uint8_t ep = req->wIndex;

	/* Only support ENDPOINT_HALT feature on non-zero EP that exist
	 * and only when in CONFIGURED state */
	if ((usb_get_state() < USB_DS_CONFIGURED) ||
	    (req->wValue != 0) ||	/* ENDPOINT_HALT */
	    (ep == 0) ||
	    (!usb_ep_is_configured(ep)))
		return false;

	/* Resume the EP */
	return usb_ep_resume(ep);
}

static bool
_set_feature_dev(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	/* No support for any device feature */
	return false;
}

static bool
_set_feature_intf(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	/* No support for any interface feature */
	return false;
}

static bool
_set_feature_ep(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	uint8_t ep = req->wIndex;

	/* Only support ENDPOINT_HALT feature on non-zero EP that exist
	 * and only when in CONFIGURED state */
	if ((usb_get_state() < USB_DS_CONFIGURED) ||
	    (req->wValue != 0) ||	/* ENDPOINT_HALT */
	    (ep == 0) ||
	    (!usb_ep_is_configured(ep)))
		return false;

	/* Halt the EP */
	return usb_ep_halt(ep);
}

static bool
_set_addr_done(struct usb_xfer *xfer)
{
	struct usb_ctrl_req *req = xfer->cb_ctx;
	usb_set_address(req->wValue);
	return true;
}

static bool
_set_address(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	xfer->len = 0;
	xfer->cb_done = _set_addr_done;
	xfer->cb_ctx = req;
	return true;
}

static bool
_get_descriptor(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	int idx = req->wValue & 0xff;

	xfer->data = NULL;

	switch (req->wValue & 0xff00)
	{
	case 0x0100:	/* Device */
		xfer->data = (void*)g_usb.stack_desc->dev;
		xfer->len  = g_usb.stack_desc->dev->bLength;
		break;

	case 0x0200:	/* Configuration */
		if (idx < g_usb.stack_desc->n_conf) {
			xfer->data = (void*)g_usb.stack_desc->conf[idx];
			xfer->len  = g_usb.stack_desc->conf[idx]->wTotalLength;
		}
		break;

	case 0x0300:	/* String */
		if (idx < g_usb.stack_desc->n_str) {
			xfer->data = (void*)g_usb.stack_desc->str[idx];
			xfer->len  = g_usb.stack_desc->str[idx]->bLength;
		}
		break;

	case 0x0f00:	/* BOS */
		if (g_usb.stack_desc->bos) {
			xfer->data = (void*)g_usb.stack_desc->bos;
			xfer->len  = g_usb.stack_desc->bos->wTotalLength;
		}
	}

	return xfer->data != NULL;
}

static bool
_get_configuration(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	xfer->data[0] = g_usb.conf ? g_usb.conf->bConfigurationValue : 0;
	xfer->len = 1;
	return true;
}

static bool
_set_configuration(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	const struct usb_conf_desc *conf = NULL;
	enum usb_dev_state new_state;

	const struct usb_intf_desc *intf;
	const void *sod, *eod;

	/* Handle the 'zero' case first */
	if (req->wValue == 0) {
		new_state = USB_DS_DEFAULT;
	} else {
		/* Find the requested config */
		for (int i=0; i<g_usb.stack_desc->n_conf; i++)
			if (g_usb.stack_desc->conf[i]->bConfigurationValue == req->wValue) {
				conf = g_usb.stack_desc->conf[i];
				break;
			}

		if (!conf)
			return false;

		new_state = USB_DS_CONFIGURED;
	}

	/* Update state */
		/* FIXME: configure all endpoint */
	g_usb.conf = conf;
	g_usb.intf_alt = 0;
	usb_set_state(new_state);
	usb_dispatch_set_conf(g_usb.conf);

	/* Dispatch implicit set_interface alt 0 */
	sod = conf;
	eod = sod + conf->wTotalLength;

	while (1) {
		sod = usb_desc_find(sod, eod, USB_DT_INTF);
		if (!sod)
			break;

		intf = (void*)sod;
		if (intf->bAlternateSetting == 0)
			usb_dispatch_set_intf(intf, intf);

		sod = usb_desc_next(sod);
	}

	return true;
}

static bool
_get_interface(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	const struct usb_intf_desc *intf;
	uint8_t idx = req->wIndex;
	uint8_t alt = 0;
	enum usb_fnd_resp rv;

	/* Check interface exits */
	intf = usb_desc_find_intf(NULL, idx, 0, NULL);
	if (intf == NULL)
		return false;

	/* Fast path */
	if (!(g_usb.intf_alt & (1 << idx))) {
		xfer->data[0] = 0x00;
		xfer->len = 1;
		return true;
	}

	/* Dispatch for an answer */
	rv = usb_dispatch_get_intf(intf, &alt);
	if (rv != USB_FND_SUCCESS)
		return false;

	/* Setup response */
	xfer->data[0] = alt;
	xfer->len = 1;

	return true;
}

static bool
_set_interface(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	const struct usb_intf_desc *intf_base, *intf_alt;
	uint8_t idx = req->wIndex;
	uint8_t alt = req->wValue;
	enum usb_fnd_resp rv;

	/* Check interface exits and its altsettings */
	intf_alt = usb_desc_find_intf(NULL, req->wIndex, alt, &intf_base);
	if (intf_alt == NULL)
		return false;

	/* Dispatch enable */
	rv = usb_dispatch_set_intf(intf_base, intf_alt);
	if (rv != USB_FND_SUCCESS)
		return false;

	/* Disable fast path if (alt != 0) & success */
	if (alt != 0)
		g_usb.intf_alt |= (1 << idx);

	return true;
}


	/* Control Request dispatch */

static enum usb_fnd_resp
usb_ctrl_std_handle(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	bool rv = false;

	/* Main dispatch */
	switch (req->wRequestAndType)
	{
	case USB_RT_GET_STATUS_DEV:
		rv = _get_status_dev(req, xfer);
		break;

	case USB_RT_GET_STATUS_INTF:
		rv = _get_status_intf(req, xfer);
		break;

	case USB_RT_GET_STATUS_EP:
		rv = _get_status_ep(req, xfer);
		break;

	case USB_RT_CLEAR_FEATURE_DEV:
		rv = _clear_feature_dev(req, xfer);
		break;

	case USB_RT_CLEAR_FEATURE_INTF:
		rv = _clear_feature_intf(req, xfer);
		break;

	case USB_RT_CLEAR_FEATURE_EP:
		rv = _clear_feature_ep(req, xfer);
		break;

	case USB_RT_SET_FEATURE_DEV:
		rv = _set_feature_dev(req, xfer);
		break;

	case USB_RT_SET_FEATURE_INTF:
		rv = _set_feature_intf(req, xfer);
		break;

	case USB_RT_SET_FEATURE_EP:
		rv = _set_feature_ep(req, xfer);
		break;

	case USB_RT_SET_ADDRESS:
		rv = _set_address(req, xfer);
		break;

	case USB_RT_GET_DESCRIPTOR:
		rv = _get_descriptor(req, xfer);
		break;

	case USB_RT_GET_CONFIGURATION:
		rv = _get_configuration(req, xfer);
		break;

	case USB_RT_SET_CONFIGURATION:
		rv = _set_configuration(req, xfer);
		break;

	case USB_RT_GET_INTERFACE:
		rv = _get_interface(req, xfer);
		break;

	case USB_RT_SET_INTERFACE:
		rv = _set_interface(req, xfer);
		break;

	default:
		return USB_FND_CONTINUE;
	}

	return rv ? USB_FND_SUCCESS : USB_FND_ERROR;
}

struct usb_fn_drv usb_ctrl_std_drv = {
	.ctrl_req = usb_ctrl_std_handle,
};
