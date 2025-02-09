/*
    Copyright 2000-2010 Broadcom Corporation

    <:label-BRCM:2011:DUAL/GPL:standard
    
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

#ifndef _WAN_DRV_H_
#define _WAN_DRV_H_

#include <bdmf_data_types.h>

#define wd_log(fmt, ...) printk("SERDES: %s "fmt, __FUNCTION__, ##__VA_ARGS__)
#if 0
#define wd_log_debug(fmt, ...) printk("SERDES: %s "fmt, __FUNCTION__, ##__VA_ARGS__)
#else
#define wd_log_debug(fmt, ...)
#endif

typedef enum
{
    SERDES_WAN_TYPE_NONE,
    SERDES_WAN_TYPE_GPON,
    SERDES_WAN_TYPE_EPON_1G,
    SERDES_WAN_TYPE_EPON_2G,
    SERDES_WAN_TYPE_AE,
    SERDES_WAN_TYPE_AE_2_5G,
    /* XGPON/NGPON types */
    SERDES_WAN_TYPE_AE_10G,
    SERDES_WAN_TYPE_EPON_10G_SYM,
    SERDES_WAN_TYPE_EPON_10G_ASYM,
    SERDES_WAN_TYPE_XGPON_10G_2_5G,
    SERDES_WAN_TYPE_NGPON_10G_10G,
    SERDES_WAN_TYPE_NGPON_10G_10G_8B10B,
    SERDES_WAN_TYPE_NGPON_10G_2_5G_8B10B,
    SERDES_WAN_TYPE_NGPON_10G_2_5G
} serdes_wan_type_t;

typedef enum
{
    DRV_SERDES_NO_ERROR, 
    DRV_SERDES_AMPLITUDE_IS_OUT_OF_RANGE,
}
serdes_error_t;


#define PMD_EWAKE_OFF_TIME   0x2f
#define PMD_EWAKE_SETUP_TIME 0x2f
#define PMD_EWAKE_HOLD_TIME  0x2f

int wan_serdes_config(serdes_wan_type_t wan_type);
serdes_wan_type_t wan_serdes_type_get(void);
void wan_prbs_gen(uint32_t enable, int enable_host_tracking, int mode, serdes_wan_type_t wan_type, bdmf_boolean *valid);
void wan_prbs_status(bdmf_boolean *valid, uint32_t *errors);

typedef void (*PMD_DEV_ENABLE_PRBS)(uint16_t enable, uint8_t prbs_mode);
void wan_register_pmd_prbs_callback(PMD_DEV_ENABLE_PRBS callback);


#if defined(CONFIG_BCM96858) || defined(CONFIG_BCM96836)
int ngpon_wan_post_init(uint32_t sync_frame_length);
void wan_top_tod_ts48_get(uint16_t *ts48_msb, uint32_t *ts48_lsb);
void ngpon_wan_top_enable_transmitter(int enabled);
void gpon_ngpon_wan_top_set_lbe_invert(uint8_t lbe_invert_bit);
void ngpon_wan_top_reset_gearbox_rx(void);
#endif

#ifdef CONFIG_BCM96848
void onu2g_read(uint32_t addr, uint16_t *data);
void onu2g_write(uint32_t addr, uint16_t data);
#endif

#ifdef CONFIG_BCM96838
serdes_error_t wan_serdes_amplitude_set(uint16_t val);
serdes_error_t wan_serdes_amplitude_get(uint16_t *val);
#endif

typedef struct pon_serdes_fixup_fifo_t
{
    void (*reset_fifo)(void);
    int (*gearbox_drift_test)(void);
} pon_serdes_lof_fixup_fifo_t;
typedef void (*pon_serdes_lof_fixup_reset_cdr_t)(void);
int pon_serdes_lof_fixup_cfg(pon_serdes_lof_fixup_fifo_t *_fifo_cb, pon_serdes_lof_fixup_reset_cdr_t reset_cb);
#endif

