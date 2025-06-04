/*
 * usb_dfu_rt.c
 *
 * Copyright (C) 2019-2021  Sylvain Munaut <tnt@246tNt.com>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <no2usb/usb.h>
#include <no2usb/usb_dfu_rt.h>
#include <no2usb/usb_dfu_proto.h>


#define DFU_POLL_MS		250

static int g_dfu_rt_intf;

static bool
_dfu_detach_done_cb(struct usb_xfer *xfer)
{
	usb_dfu_rt_cb_reboot();
	return true;
}

static enum usb_fnd_resp
_dfu_ctrl_req(struct usb_ctrl_req *req, struct usb_xfer *xfer)
{
	/* If this a class request for DFU interface ? */
	if ((USB_REQ_TYPE(req) | USB_REQ_RCPT(req)) != (USB_REQ_TYPE_CLASS | USB_REQ_RCPT_INTF))
		return USB_FND_CONTINUE;

	if (req->wIndex != g_dfu_rt_intf)
		return USB_FND_CONTINUE;

	/* Handle request */
	switch (req->wRequestAndType)
	{
	case USB_RT_DFU_DETACH:
		xfer->cb_done = _dfu_detach_done_cb;
		break;

	case USB_RT_DFU_GETSTATUS:
		/* Return data */
		xfer->data[0] = OK;
		xfer->data[1] = (DFU_POLL_MS >>  0) & 0xff;
		xfer->data[2] = (DFU_POLL_MS >>  8) & 0xff;
		xfer->data[3] = (DFU_POLL_MS >> 16) & 0xff;
		xfer->data[4] = appIDLE;
		xfer->data[5] = 0;
		break;

	case USB_RT_DFU_GETSTATE:
		/* Return state */
		xfer->data[0] = appIDLE;
		break;

	default:
		return USB_FND_ERROR;
	}

	return USB_FND_SUCCESS;
}

static enum usb_fnd_resp
_dfu_set_intf(const struct usb_intf_desc *base, const struct usb_intf_desc *sel)
{
	if ((sel->bInterfaceClass != 0xfe) ||
	    (sel->bInterfaceSubClass != 0x01) ||
	    (sel->bInterfaceProtocol != 0x01))
		return USB_FND_CONTINUE;

	g_dfu_rt_intf = base->bInterfaceNumber;

	return USB_FND_SUCCESS;
}


static struct usb_fn_drv _dfu_rt_drv = {
	.ctrl_req	= _dfu_ctrl_req,
	.set_intf	= _dfu_set_intf,
};


void __attribute__((weak))
usb_dfu_rt_cb_reboot(void)
{
	/* Nothing */
}

void
usb_dfu_rt_init(void)
{
	usb_register_function_driver(&_dfu_rt_drv);
	g_dfu_rt_intf = -1;
}
