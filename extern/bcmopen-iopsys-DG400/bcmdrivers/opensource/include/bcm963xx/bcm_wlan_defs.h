/*
<:copyright-BRCM:2016:DUAL/GPL:standard

   Copyright (c) 2016 Broadcom 
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
#ifndef _BCM_WLAN_DEFS_H_
#define _BCM_WLAN_DEFS_H_

/* XXX must be sync with WL_NUM_OF_SSID_PER_UNIT in WLAN driver */
#define SSID_PER_RADIO 8

#define WLAN_RADIO_GET(subunit) (uint32_t)((subunit) / SSID_PER_RADIO)
#define WLAN_SSID_GET(subunit)  ((subunit) % SSID_PER_RADIO)

#endif

