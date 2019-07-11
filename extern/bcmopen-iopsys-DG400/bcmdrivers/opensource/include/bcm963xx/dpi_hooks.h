#ifndef __DPI_HOOKS_H_INCLUDED__
#define __DPI_HOOKS_H_INCLUDED__
/*
<:copyright-BRCM:2014:DUAL/GPL:standard

   Copyright (c) 2014 Broadcom 
   All Rights Reserved

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2, as published by
the Free Software Foundation (the "GPL").

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.


A copy of the GPL is available at http://www.broadcom.com/licenses/GPLv2.php, or by
writing to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.

:> 
*/
#include <bcmdpi.h>

typedef struct {
    int (* nl_handler)(struct sk_buff *skb);
    int (* cpu_enqueue)(pNBuff_t pNBuff, struct net_device *dev);
} dpi_hooks_t;

int dpi_bind(dpi_hooks_t *hooks_p);

void dpi_info_get(void *conn_p, unsigned int *app_id_p, uint16_t *dev_key_p);
void dpi_blog_key_get(void *conn_p, unsigned int *blog_key_0_p, unsigned int *blog_key_1_p);

void dpi_nl_msg_reply(struct sk_buff *skb, DpictlNlMsgType_t msgType,
                      unsigned short msgLen, void *msgData_p);

int dpi_cpu_enqueue(pNBuff_t pNBuff, struct net_device *dev);

#endif /* __DPI_HOOKS_H_INCLUDED__ */
