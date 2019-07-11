/*
* <:copyright-BRCM:2016:DUAL/GPL:standard
* 
*    Copyright (c) 2016 Broadcom 
*    All Rights Reserved
* 
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as published by
* the Free Software Foundation (the "GPL").
* 
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
* 
* 
* A copy of the GPL is available at http://www.broadcom.com/licenses/GPLv2.php, or by
* writing to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
* Boston, MA 02111-1307, USA.
* 
* :> 
*/


#ifndef _BOARD_EXTIRQ_H_
#define _BOARD_EXIIRQ_H_

#include <bcm_intr.h>

#if defined(CONFIG_BCM96838) || defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148)
#define NUM_EXT_INT    (INTERRUPT_ID_EXTERNAL_5-INTERRUPT_ID_EXTERNAL_0+1)
#elif defined(CONFIG_BCM963381) || defined(CONFIG_BCM96848)
#define NUM_EXT_INT    (INTERRUPT_ID_EXTERNAL_7-INTERRUPT_ID_EXTERNAL_0+1)
#elif defined(CONFIG_BCM94908) ||  (CONFIG_BCM_CHIP_REV==0x6858A0)
#define NUM_EXT_INT    (INTERRUPT_PER_EXT_5-INTERRUPT_PER_EXT_0+1)
#elif defined(CONFIG_BCM963158) || defined(CONFIG_BCM96836)
#define NUM_EXT_INT    (INTERRUPT_PER_EXT_7-INTERRUPT_PER_EXT_0+1)
#elif (defined(CONFIG_BCM96858) && (CONFIG_BCM_CHIP_REV != 0x6858A0))
#define NUM_EXT_INT    8  
#elif defined(CONFIG_BCM947189)
#define NUM_EXT_INT    0
#else
#define NUM_EXT_INT    (INTERRUPT_ID_EXTERNAL_3-INTERRUPT_ID_EXTERNAL_0+1)
#endif

extern unsigned int extIntrInfo[NUM_EXT_INT];

int map_external_irq (int irq);
void init_ext_irq_info(void);
void init_reset_irq(void);
void board_extirq_init(void);
#if defined(CONFIG_BCM960333)
void mapBcm960333GpioToIntr( unsigned int gpio, unsigned int extIrq );
#endif
#endif
