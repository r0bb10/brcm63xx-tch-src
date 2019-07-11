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

#include "sf2.h"

static int port_sf2_port_init(enetx_port_t *self)
{
    return 0;
}

static int port_sf2_port_uninit(enetx_port_t *self)
{
    return 0;
}

static int port_sf2_sw_init(enetx_port_t *self)
{
    /* TODO: Initialize SF2
    ...
    pmc_switch_power_up();

    // power on and reset the quad and single phy
    phy_ctrl = ETHSW_REG->qphy_ctrl;
    phy_ctrl &= ~(ETHSW_QPHY_CTRL_IDDQ_BIAS_MASK|ETHSW_QPHY_CTRL_EXT_PWR_DOWN_MASK|ETHSW_QPHY_CTRL_PHYAD_BASE_MASK);
    phy_ctrl |= ETHSW_QPHY_CTRL_RESET_MASK|(phy_base<<ETHSW_QPHY_CTRL_PHYAD_BASE_SHIFT);
    ....
    */

    return 0;
}

static int port_sf2_sw_uninit(enetx_port_t *self)
{
}

static int port_sf2_sw_demux(enetx_port_t *sw, int rx_port, FkBuff_t *fkb, enetx_port_t **out_port)
{
    return 0;
}

static int port_sf2_sw_mux(enetx_port_t *tx_port, FkBuff_t *fkb, enetx_port_t **out_port)
{
    return 0;
}

struct sw_ops port_sf2_sw =
{
    .init = &port_sf2_sw_init,
    .uninit = &port_sf2_sw_uninit,
    .port_demux = &port_sf2_sw_demux,
    .port_mux = &port_sf2_sw_mux,
};

struct port_ops port_sf2_port =
{
    .init = &port_sf2_port_init,
    .uninit = &port_sf2_port_uninit,
};

