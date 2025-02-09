/*
   <:copyright-BRCM:2015:DUAL/GPL:standard
   
      Copyright (c) 2015 Broadcom 
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

/*
 *  Created on: Nov/2015
 *      Author: ido@broadcom.com
 */

#include "g9991.h"
#include "port.h"
#include <rdpa_api.h>
#include <linux/skbuff.h>
#include "enet_dbg.h"

extern int wan_port_id;

bdmf_object_handle create_rdpa_port(rdpa_if rdpaif, rdpa_emac emac, bdmf_object_handle owner);
extern int link_pbit_tc_to_q_to_rdpa_lan_port(bdmf_object_handle port_obj);

static int create_rdpa_g9991_port(enetx_port_t *self, rdpa_if rdpaif)
{
    bdmf_error_t rc = 0;
    bdmf_object_handle port_obj = NULL;
    bdmf_object_handle switch_port_obj = NULL;
    rdpa_port_dp_cfg_t port_cfg = {};
    int physical_port = self->p.parent_sw->s.parent_port->p.mac->mac_id;

    if ((rc = rdpa_port_get(rdpa_if_switch, &switch_port_obj)))
    {
        enet_err("Failed to get RDPA switch port object. rc=%d\n", rc);
        goto Exit;
    }

    if (!(self->priv = port_obj = create_rdpa_port(rdpaif, rdpa_emac0 + physical_port, switch_port_obj)))
    {
        enet_err("Failed to create RDPA port %d ret=%d\n", rdpaif, (rc = -1));
        goto Exit;
    }

    if ((rc = rdpa_port_cfg_get(port_obj, &port_cfg)))
    {
        enet_err("Failed to get configuration for RDPA port %d. rc=%d\n", rdpaif, rc);
        goto Exit;
    }

    port_cfg.physical_port = physical_port;

    if ((rc = rdpa_port_cfg_set(port_obj, &port_cfg)))
    {
        enet_err("Failed to set configuration for RDPA port %d. rc=%d\n", rdpaif, rc);
        goto Exit;
    }
    
    rc = link_pbit_tc_to_q_to_rdpa_lan_port(self->priv);
    if (rc)
        return rc;

    enet_dbg("Created RDPA g9991 port %d\n", rdpaif);

Exit:
    if (rc)
        enet_err("Failed to create RDPA port object (rc=%d) for %s\n", rc, self->obj_name);

    if (switch_port_obj)
        bdmf_put(switch_port_obj);

    return rc;
}

static int port_g9991_port_init(enetx_port_t *self)
{
    bdmf_error_t rc;
    rdpa_if rdpaif = self->p.port_id;
    int i;

#ifdef CONFIG_BLOG
    self->n.blog_phy = BLOG_SIDPHY;
    self->n.blog_chnl = rdpaif - rdpa_if_lan0;
#endif
    
    if (demux_on_sw(root_sw, rdpaif, self))
        return -1;

    if ((rc = create_rdpa_g9991_port(self, rdpaif)))
        return -1;

    if (rdpaif - rdpa_if_lan0 != G9991_MGMT_SID)
        goto Exit;

    for (i = G9991_MGMT_SID + 1; i <= G9991_MGMT_SID_MAX; i++)
    {
        enet_dbg("Initialized demux_map for sid %d\n", i);
        if (demux_on_sw(root_sw, rdpa_if_lan0 + i, self))
            return -1;
    }

Exit:
    enet_dbg("Initialized g9991 port %s\n", self->obj_name);

    return 0;
}

static int _rdpa_destroy_object(bdmf_object_handle *rdpa_obj)
{
    if (*rdpa_obj)
    {
        enet_dbg("destroyed RDPA port %s\n", (*rdpa_obj)->name);
        bdmf_destroy(*rdpa_obj);
        *rdpa_obj = NULL;
    }

    return 0;
}

static int port_g9991_port_uninit(enetx_port_t *self)
{
    demux_on_sw(root_sw, self->p.port_id, NULL);

    return _rdpa_destroy_object((bdmf_object_handle *)&self->priv);
}

static int port_g9991_sw_init(enetx_port_t *self)
{
    int physical_port;
    enetx_port_t *p;
    port_info_t port_info =
    {
        .port = G9991_MGMT_SID,
    };

    physical_port = self->s.parent_port->p.mac->mac_id;
    port_open(self->s.parent_port);

    if (physical_port != G9991_MGMT_SID_LINK)
        goto Exit;

    if (!(p = port_create(&port_info, self)))
    {
        enet_err("Failed to create g9991 dsp port\n");
        return -1;
    }

    p->has_interface = 1;
    strncpy(p->name, "dsp0", IFNAMSIZ);

Exit:
    enet_dbg("Initialized g9991 switch %s\n", self->obj_name);

    return 0;
}

static int port_g9991_sw_uninit(enetx_port_t *self)
{
    port_stop(self->s.parent_port);
    return _rdpa_destroy_object((bdmf_object_handle *)&self->priv);
}

#ifdef CONFIG_BCM_XRDP
extern int port_runner_dispatch_pkt_lan(dispatch_info_t *dispatch_info);
#else
static int dispatch_pkt_lan_bridge(dispatch_info_t *dispatch_info)
{
    int rc;
    rdpa_cpu_tx_info_t info = {};

    info.method = rdpa_cpu_tx_bridge;
    info.port = rdpa_if_wan0;
    /* XXX: send to reserved gemflow */
    info.x.wan.flow = RDPA_MAX_GEM_FLOW + dispatch_info->port->p.port_id - rdpa_if_lan0;
    info.x.wan.queue_id = 0; /* XXX: Temp. limitation default flow to queue 0 */
    info.drop_precedence = dispatch_info->drop_eligible;
    enet_dbg_tx("rdpa_cpu_send_sysb: flow %d\n", info.x.wan.flow);

    /* FTTDP FW does not support sending from sysb, so we need to copy to bpm */
    rc = rdpa_cpu_send_raw(bdmf_sysb_data(dispatch_info->pNBuff),
      bdmf_sysb_length(dispatch_info->pNBuff), &info);
    /* rdpa_cpu_send_raw copies to bpm but does not free buffer */
    nbuff_flushfree(dispatch_info->pNBuff);

    return rc;
}
#endif

static void port_g9991_port_stats_clear(enetx_port_t *self)
{
    bdmf_object_handle port_obj = self->priv;
    rdpa_port_stat_t port_stat = {};

    rdpa_port_stat_set(port_obj, &port_stat);
}

static void port_g9991_port_stats_get(enetx_port_t *self, struct rtnl_link_stats64 *net_stats)
{
    bdmf_object_handle port_obj = self->priv;
    rdpa_port_stat_t port_stat;

    rdpa_port_stat_get(port_obj, &port_stat);

    net_stats->multicast = port_stat.rx_multicast_pkt;
    net_stats->rx_bytes = port_stat.rx_valid_bytes;
    net_stats->rx_packets = port_stat.rx_valid_pkt;
    net_stats->tx_bytes = port_stat.tx_valid_bytes;
    net_stats->tx_packets = port_stat.tx_valid_pkt;
#if defined(CONFIG_BCM_KF_EXTSTATS)
    net_stats->tx_multicast_packets = port_stat.tx_multicast_pkt;
    net_stats->rx_broadcast_packets = port_stat.rx_broadcast_pkt;
    net_stats->tx_broadcast_packets = port_stat.tx_broadcast_pkt;
#endif
}

static int port_g9991_sw_port_id_on_sw(port_info_t *port_info, int *port_id, port_type_t *port_type)
{
    *port_id = rdpa_if_lan0 + port_info->port;
    *port_type = PORT_TYPE_G9991_PORT;

    return 0;
}

sw_ops_t port_g9991_sw =
{
    .init = port_g9991_sw_init,
    .uninit = port_g9991_sw_uninit,
    .port_id_on_sw = port_g9991_sw_port_id_on_sw,
};

port_ops_t port_g9991_port =
{
    .init = port_g9991_port_init,
    .uninit = port_g9991_port_uninit,
#ifdef CONFIG_BCM_XRDP
    .dispatch_pkt = port_runner_dispatch_pkt_lan,
#else
    .dispatch_pkt = dispatch_pkt_lan_bridge,
#endif
    .stats_get = port_g9991_port_stats_get,
    .stats_clear = port_g9991_port_stats_clear,
};

