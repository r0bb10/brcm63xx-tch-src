/*
   Copyright (c) 2015 Broadcom Corporation
   All Rights Reserved

    <:label-BRCM:2015:DUAL/GPL:standard
    
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
 *  Created on: Dec 2015
 *      Author: yuval.raviv@broadcom.com
 */

#include "bp_parsing.h"
#include "enet_dbg.h"
#include "phy_drv.h"
#include "mac_drv.h"
#include "phy_bp_parsing.h"
#include "boardparms.h"

#define SWITCH_UNIT_ROOT 0
#define SWITCH_UNIT_EXTERNAL 1

static int __init bp_parse_port(enetx_port_t *sw, const ETHERNET_MAC_INFO *emac_info, uint32_t port, uint32_t unit);

extern enetx_port_t *unit_port_array[BP_MAX_ENET_MACS][BP_MAX_SWITCH_PORTS];
extern int unit_port_oam_idx_array[BP_MAX_ENET_MACS][BP_MAX_SWITCH_PORTS];

static int __init bp_parse_port_cap(const ETHERNET_MAC_INFO *emac_info, uint32_t port)
{
    int port_flags = emac_info->sw.port_flags[port];

    if (port_flags & PORT_FLAG_MGMT)
        return PORT_CAP_MGMT;

    switch (port_flags & PORT_FLAG_LANWAN_M)
    {
    case PORT_FLAG_WAN_ONLY:
        return PORT_CAP_WAN_ONLY;
    case PORT_FLAG_WAN_PREFERRED:
        return PORT_CAP_WAN_PREFERRED;
    case PORT_FLAG_LAN_ONLY:
        return PORT_CAP_LAN_ONLY;
    default:
        break;
    }

    return PORT_CAP_LAN_WAN;
}

static enetx_port_t __init *bp_parse_attached_port(enetx_port_t *sw, int sid, const char *dev_name, uint32_t port)
{
    enetx_port_t *p;
    port_info_t port_info =
    {
        .port = sid,
    };

    if (!(p = port_create(&port_info, sw)))
    {
        enet_err("Failed to create g9991 port: port=%d sid=%d\n", port, sid);
        return NULL;
    }

    p->has_interface = 1;
    p->p.port_cap = PORT_CAP_LAN_ONLY;

    if (dev_name)
        strncpy(p->name, dev_name, IFNAMSIZ);
    else
        snprintf(p->name, IFNAMSIZ, "sid%d", sid);

    enet_dbg("Created g9991 port: name=%s port=%d sid=%d\n", dev_name, port, sid);

    /* Add translation arry - port-> unit,port */
    unit_port_array[0][sid] = p;
    enet_dbg("backward compat unit %d port %d -> %s\n", 0, sid, p->obj_name);
    /* keep backword compatible oam_idx mapping */
    unit_port_oam_idx_array[0][sid] = sid;
    enet_dbg("backward oam unit %d port %d -> %d\n", 0, sid, unit_port_oam_idx_array[0][sid]);

    return p;
}

static enetx_port_t *__init bp_create_sw(port_type_t type, char *name, enetx_port_t *parent_port)
{
    enetx_port_t *sw;

    if (!(sw = sw_create(type, parent_port)))
        return NULL;

    if (name)
    {
        sw->has_interface = 1;
        strncpy(sw->name, name, IFNAMSIZ);
    }

    return sw;
}

static int __init bp_parse_attached(enetx_port_t *parent_port, uint32_t port)
{
    int i;
    enetx_port_t *sw;
    BP_ATTACHED_INFO bp_attached_info;

    if (BpGetAttachedInfo(port, &bp_attached_info))
    {
        enet_err("Failed to get attached ports for port %d\n", port);
        return -1;
    }

    sw = bp_create_sw(PORT_TYPE_G9991_SW, NULL, parent_port);
    if (!sw)
        return -1;

    for (i = 0; i < BP_MAX_ATTACHED_PORTS; i++)
    {
        if (!(bp_attached_info.port_map & 1<<i))
            continue;

        if (!bp_parse_attached_port(sw, bp_attached_info.ports[i], bp_attached_info.devnames[i], port))
            return -1;
    }

    return 0;
}

static int __init bp_parse_unit(enetx_port_t *parent_port, int unit)
{
    const ETHERNET_MAC_INFO *emac_info;
    port_type_t type;
    char *ifname = NULL;
    uint32_t port;
    enetx_port_t *sw;

    if ((emac_info = BpGetEthernetMacInfoArrayPtr()) == NULL)
    {
        enet_err("Error reading Ethernet MAC info from board params\n");
        return -1;
    }

    switch (emac_info[unit].ucPhyType)
    {
    case BP_ENET_INTERNAL_PHY:
        type = PORT_TYPE_RUNNER_SW;
        ifname = "bcmsw";
        break;
    case BP_ENET_NO_PHY:
        /* XXX DSL Runner ports */
        type = PORT_TYPE_DIRECT_RGMII;
        break;
    case BP_ENET_EXTERNAL_SWITCH:
        /* XXX DSL Star Fighter 2 external switch */
        /* type = PORT_TYPE_SF2_SW; */
        ifname = "bcmsw";
    case BP_ENET_SWITCH_VIA_INTERNAL_PHY:
        /* XXX No one uses this type */
    default:
        enet_err("cannot find sw object match for boardparms unit type %d\n", emac_info[unit].ucPhyType);
        return -1;
    }

    sw = bp_create_sw(type, ifname, parent_port);
    if (unit == 0)
        root_sw = sw;

    if (!sw)
        return -1;

    for (port = 0; port < BP_MAX_SWITCH_PORTS; port++)
    {
        if (!(emac_info->sw.port_map & (1<<port)))
            continue;

        if (bp_parse_port(sw, emac_info, port, unit))
            return -1;
    }

    return 0;
}

static int __init bp_parse_port(enetx_port_t *sw, const ETHERNET_MAC_INFO *emac_info, uint32_t port, uint32_t unit)
{
    enetx_port_t *p;
    int port_flags = emac_info->sw.port_flags[port];
    int is_wan_only = port_flags & PORT_FLAG_WAN_ONLY ? 1 : 0;
    int is_attached = port_flags & PORT_FLAG_ATTACHED ? 1 : 0;
    char *dev_name = emac_info->sw.phy_devName[port];
    int ext_switch = IsPortConnectedToExternalSwitch(emac_info->sw.phy_id[port]);
    int is_management = port_flags & PORT_FLAG_MGMT ? 1 : 0;
    int oam_idx = emac_info->sw.oamIndex[port];
    port_info_t port_info =
    {
        .port = port,
        .is_management = is_management,
        .is_attached = is_attached,
        .is_detect = is_wan_only, /* XXX: Should add detect flag to boardparms */
    };
    
    oam_idx = oam_idx; /* Satisfy compiler warning */

    if (!(p = port_create(&port_info, sw)))
    {
        enet_err("Failed to create unit %d port %d\n", unit, port);
        return -1;
    }

    p->p.mac = bp_parse_mac_dev(emac_info, port); 
    p->p.phy = bp_parse_phy_dev(emac_info, port); 

    if (is_attached)
    {
        if (bp_parse_attached(p, port))
        {
            enet_err("Failed to parse attached ports for port %d\n", port);
            return -1;
        }
    }
    else if (ext_switch)
    {
        if (bp_parse_unit(p, SWITCH_UNIT_EXTERNAL))
        {
            enet_err("Failed to parse external switch ports for unit %d port %d\n", unit, port);
            return -1;
        }
    }
    else
    {
        p->p.port_cap = bp_parse_port_cap(emac_info, port);
#ifdef VLANTAG /* vlan tag port example: attach to physical port */
        {
            port_info_t port_info =
            {
                .port = 0, // vlan tag
            };

            sw = sw_create(PORT_TYPE_VLAN_SW, p);
            if (!sw || !(p = port_create(&port_info, sw))) /* overwrites *p, still used below */
            {
                enet_err("Failed to vlan sw/port for unit %d port %d\n", unit, port);
                return -1;
            }
        }
#endif

        p->has_interface = 1;
        /* Create all ports as LAN by default */
        if (!is_wan_only)
            p->n.port_netdev_role = PORT_NETDEV_ROLE_LAN;
        else
            p->n.port_netdev_role = PORT_NETDEV_ROLE_WAN;

        if (dev_name)
            strncpy(p->name, dev_name, IFNAMSIZ);
    }
    
#ifndef BRCM_FTTDP
    /* Add translation arry - port-> unit,port */
    unit_port_array[unit][port] = p;
    enet_dbg("backward compat unit %d port %d -> %s\n", unit, port, p->obj_name);
    /* keep backword compatible oam_idx mapping */
    unit_port_oam_idx_array[unit][port] = (oam_idx >= 0) ? oam_idx : (unit == 0) ? port : -1;
    enet_dbg("backward oam unit %d port %d -> %d\n", unit, port, unit_port_oam_idx_array[unit][port]);
#endif

    return 0;
}

int __init bp_parse(void)
{
    char boardIdStr[20];

    BpGetBoardId(boardIdStr);
    enet_dbg("Parsing board configuration for %s\n", boardIdStr);

    return bp_parse_unit(NULL, SWITCH_UNIT_ROOT);
}

