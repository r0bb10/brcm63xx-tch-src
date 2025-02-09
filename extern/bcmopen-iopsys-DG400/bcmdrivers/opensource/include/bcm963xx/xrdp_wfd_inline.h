#ifndef __XRDP_WFD_INLINE_H_INCLUDED__
#define __XRDP_WFD_INLINE_H_INCLUDED__

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

Author: kosta.sopov@broadcom.com
*/

#include "rdpa_api.h"
#include "rdp_cpu_ring_defs.h"
#include "rdp_mm.h"
#include "linux/prefetch.h"
#include "rdp_cpu_ring.h"
#include "bcm_wlan_defs.h"

typedef struct {
    int qid;
    int wfd_idx;
} wfd_rx_isr_context_t;

typedef union {
    struct {
        uint16_t flowring_idx:10;
        uint16_t tx_prio:3;
        uint16_t reserved:3;
    };
    uint16_t hword;
    uint16_t ssid_vector;
} wl_metadata_dongle_t;

typedef union {
    struct {
        uint16_t chain_id:8;
        uint16_t tx_prio:4;
        uint16_t reserved:4;
    };
    uint16_t hword;
    uint16_t ssid_vector;
} wl_metadata_nic_t;

#define WFD_WLAN_QUEUE_MAX_SIZE (RDPA_CPU_WLAN_QUEUE_MAX_SIZE)

static int wfd_runner_tx(struct sk_buff *skb);

static inline void map_ssid_vector_to_ssid_index(uint16_t *bridge_port_ssid_vector, uint32_t *wifi_drv_ssid_index)
{
   *wifi_drv_ssid_index = __ffs(*bridge_port_ssid_vector);
}

static inline void wfd_dev_rx_isr_callback(long priv)
{
    wfd_rx_isr_context_t *ctx = (wfd_rx_isr_context_t *)priv;

    /* Disable PCI interrupt */
    rdpa_cpu_int_disable(rdpa_cpu_wlan0 + wfd_objects[ctx->wfd_idx].wl_radio_idx, ctx->qid);
    rdpa_cpu_int_clear(rdpa_cpu_wlan0 +  wfd_objects[ctx->wfd_idx].wl_radio_idx, ctx->qid);

    /*Atomically set the queue bit on*/
    set_bit(ctx->qid, &wfd_objects[ctx->wfd_idx].wfd_rx_work_avail);

    /* Call the RDPA receiving packets handler (thread or tasklet) */
    WFD_WAKEUP_RXWORKER(ctx->wfd_idx);
}

static inline int wfd_get_minQIdx(int wfd_idx)
{
    if (wfd_idx >= WFD_MAX_OBJECTS)
    {
        printk("%s wfd_idx %d out of bounds(%d)\n", __FUNCTION__, wfd_idx, WFD_MAX_OBJECTS);
        return -1;
    }

    return 0;
}

static inline int wfd_get_maxQIdx(int wfd_idx)
{
    if (wfd_idx >= WFD_MAX_OBJECTS)
    {
        printk("%s wfd_idx %d out of bounds(%d)\n", __FUNCTION__, wfd_idx, WFD_MAX_OBJECTS);
        return -1;
    }
    return WFD_NUM_QUEUES_PER_WFD_INST - 1;
}

static inline int wfd_config_rx_queue(int wfd_idx, int qid, uint32_t qsize,
                                      enumWFD_WlFwdHookType eFwdHookType)
{
    rdpa_cpu_rxq_cfg_t rxq_cfg;
    bdmf_object_handle rdpa_cpu_obj;
    uint32_t *ring_base = NULL;
    int rc = 0;
    bdmf_sysb_type qsysb_type = bdmf_sysb_skb;

    if (eFwdHookType == WFD_WL_FWD_HOOKTYPE_FKB)
    {
        qsysb_type = bdmf_sysb_fkb;
    }

    if (rdpa_cpu_get(rdpa_cpu_wlan0 + wfd_objects[wfd_idx].wl_radio_idx, &rdpa_cpu_obj))
        return -1;

    /* Read current configuration, set new drop threshold and ISR and write back. */
    bdmf_lock();
    rc = rdpa_cpu_rxq_cfg_get(rdpa_cpu_obj, qid, &rxq_cfg);
    if (rc)
        goto unlock_exit;

    if (qsize)
    {
        wfd_rx_isr_context_t *isr_ctx = (wfd_rx_isr_context_t *)kmalloc(GFP_KERNEL, sizeof(wfd_rx_isr_context_t));

        isr_ctx->qid = qid;
        isr_ctx->wfd_idx = wfd_idx;
        rxq_cfg.isr_priv = (long)isr_ctx;
    }
    else
    {
        kfree((wfd_rx_isr_context_t *)rxq_cfg.isr_priv);
        rxq_cfg.isr_priv = 0;
    }

    rxq_cfg.size = qsize ? WFD_WLAN_QUEUE_MAX_SIZE : 0;
    rxq_cfg.rx_isr = qsize ? wfd_dev_rx_isr_callback : 0;
    rxq_cfg.ring_head = ring_base;
    rxq_cfg.ic_cfg.ic_enable = qsize ? true : false;
    rxq_cfg.ic_cfg.ic_timeout_us = WFD_INTERRUPT_COALESCING_TIMEOUT_US;
    rxq_cfg.ic_cfg.ic_max_pktcnt = WFD_INTERRUPT_COALESCING_MAX_PKT_CNT;
    rxq_cfg.rxq_stat = NULL;
    rc = rdpa_cpu_rxq_cfg_set(rdpa_cpu_obj, qid, &rxq_cfg);

    if (qsize)
        rdpa_cpu_tc_to_rxq_set(rdpa_cpu_obj, qid, qid);

unlock_exit:
    bdmf_put(rdpa_cpu_obj);
    bdmf_unlock();
    return rc;
}

static void release_wfd_interfaces(void)
{
    /* TODO: implement */
}

static inline void wfd_mcast_forward(void **packets, uint32_t count, wfd_object_t *wfd_p, int is_fkb)
{
    uint32_t ifid, i;
    void *nbuf = NULL;
    int orig_packet_used;

    for (i = 0; i < count; i++)
    {
        uint16_t ssid_vector = is_fkb ? ((FkBuff_t *)(packets[i]))->wl.mcast.ssid_vector : 
            ((struct sk_buff *)packets[i])->wl.mcast.ssid_vector;

        orig_packet_used = 0;
        while (ssid_vector)
        {
            ifid = __ffs(ssid_vector);
            ssid_vector &= ~(1 << ifid);

            /* Check if device was initialized */
            if (unlikely(!wfd_p->wl_if_dev[ifid]))
            {
                printk("%s wifi_net_devices[%d] returned NULL\n", __FUNCTION__, ifid);
                continue;
            }

            wfd_p->count_rx_queue_packets++;
            wfd_p->wl_mcast_packets++;

            if (ssid_vector) /* To prevent (last/the only) ssid copy */
            {
                if (is_fkb)
                    nbuf = fkb_clone(packets[i]);
                else
                    nbuf = skb_copy(packets[i], GFP_ATOMIC);

                if (!nbuf)
                {
                    printk("%s %s: Failed to clone %ckb\n", __FILE__, __FUNCTION__, is_fkb ? 'f':'s');
                    nbuff_free(packets[i]);
                    return;
                }
            }
            else
            {
                orig_packet_used = 1;
                nbuf = packets[i];
            }

            (void)wfd_p->wfd_mcastHook(wfd_p->wl_radio_idx, (unsigned long)nbuf, (unsigned long)wfd_p->wl_if_dev[ifid]);
        }
        if (unlikely(!orig_packet_used))
            nbuff_free(packets[i]);
    }
}

static struct sk_buff *skb_alloc(rdpa_cpu_rx_info_t *info)
{
    struct sk_buff *skb;

    skb = skb_header_alloc();
    if (unlikely(!skb))
        return NULL;

    skb_headerinit(BCM_PKT_HEADROOM + info->data_offset,
#if defined(CC_NBUFF_FLUSH_OPTIMIZATION)
            SKB_DATA_ALIGN(info->size + BCM_SKB_TAILROOM + info->data_offset),
#else
            BCM_MAX_PKT_LEN - info->data_offset,
#endif
            skb, info->data + info->data_offset, bdmf_sysb_recycle, 0, NULL);

    skb_trim(skb, info->size);
    skb->recycle_flags &= SKB_NO_RECYCLE; /* no skb recycle,just do data recyle */

    return skb;
}

static inline int exception_packet_handle(struct sk_buff *skb, rdpa_cpu_rx_info_t *info, wfd_object_t *wfd_p)
{
    BlogAction_t blog_action;

    if (!wfd_p->wl_if_dev[info->dest_ssid])
        return BDMF_ERR_NODEV;

    skb->dev = wfd_p->wl_if_dev[info->dest_ssid];
    blog_action = blog_sinit(skb, skb->dev, TYPE_ETH, 0, BLOG_WLANPHY);

    if (blog_action == PKT_DONE)
        return 0;

    if (blog_action == PKT_DROP)
    {
        kfree_skb(skb);
        return 0;
    }

    if (skb->blog_p)
        skb->blog_p->rnr.is_rx_hw_acc_en = 1;

    skb->protocol = eth_type_trans(skb, skb->dev);
    skb_shinfo(skb)->dirty_p = skb->data;

    netif_receive_skb(skb);

    return BDMF_ERR_OK;
}

static inline int
_wfd_bulk_fkb_get(uint32_t qid, uint32_t budget, wfd_object_t *wfd_p, void **ucast_packets, uint32_t *_ucast_count, 
        void **mcast_packets, uint32_t *_mcast_count)
{
    FkBuff_t *fkb;
    rdpa_cpu_rx_info_t info = {};
    wl_metadata_dongle_t wl_dongle;
    int rc = 0;
    uint32_t ucast_count = 0, mcast_count = 0;

    while (budget)
    {
        rc = rdpa_cpu_packet_get(rdpa_cpu_wlan0 + wfd_p->wl_radio_idx, qid, &info);
        if (unlikely(rc))
            break;

        budget--;
        if (info.is_exception)
        {
            struct sk_buff *skb = skb_alloc(&info);

            if (unlikely(!skb))
            {
                gs_count_no_buffers[qid]++;
                continue;
            }

            rc = exception_packet_handle(skb, &info, wfd_p);
            if (unlikely(rc == BDMF_ERR_NODEV))
                gs_count_rx_no_wifi_interface[qid]++;
            continue;
        }
      
        fkb = fkb_init((uint8_t *)info.data, BCM_PKT_HEADROOM, (uint8_t*)(info.data + info.data_offset), 
                info.size);
#if defined(CC_NBUFF_FLUSH_OPTIMIZATION)
        //     fkb->dirty_p = _to_dptr_from_kptr_(data + ETH_HLEN);
#endif
        fkb->recycle_hook = bdmf_sysb_recycle;
        fkb->recycle_context = 0;
        wl_dongle.hword = info.wl_metadata;

        /* Both ucast and mcast share the same wl_prio bits position */
        if ((fkb->wl.ucast.dhd.is_ucast = info.is_ucast))
        {
            fkb->wl.ucast.dhd.ssid = info.dest_ssid;
            fkb->wl.ucast.dhd.flowring_idx = wl_dongle.flowring_idx;
            fkb->wl.ucast.dhd.wl_prio = wl_dongle.tx_prio;
            ucast_packets[ucast_count++] = (void *)fkb;
        }
        else
        {
            fkb->wl.mcast.ssid_vector = wl_dongle.ssid_vector;
            mcast_packets[mcast_count++] = (void *)fkb;
            fkb->wl.mcast.wl_prio = info.mcast_tx_prio; // this parameter always 0- in MCAST dhd_wfd_mcasthandler update priority
        }
    }

    *_ucast_count = ucast_count;
    *_mcast_count = mcast_count;
    return rc;
}

static inline uint32_t
wfd_bulk_fkb_get(unsigned long qid, unsigned long budget, void *priv, void **ucast_packets)
{
    int rc;
    wfd_object_t *wfd_p = (wfd_object_t *)priv;
    uint32_t ucast_count = 0, mcast_count = 0;
    void *mcast_packets[NUM_PACKETS_TO_READ_MAX];

    rc = _wfd_bulk_fkb_get(qid, budget, wfd_p, ucast_packets, &ucast_count, mcast_packets, &mcast_count);

    /* First handle the fastpath bulk packets */
    if (rc && rc != BDMF_ERR_NO_MORE)
        printk("%s:%d _wfd_bulk_fkb_get failed; rc %d\n", __func__, __LINE__, rc);

    if (mcast_count)
        wfd_mcast_forward(mcast_packets, mcast_count, wfd_p, 1);

    if (!ucast_count)
        return mcast_count;

    (void) wfd_p->wfd_fwdHook(ucast_count, (unsigned long)ucast_packets, wfd_p->wl_radio_idx, 0);
    wfd_p->wl_chained_packets += ucast_count;
    wfd_p->count_rx_queue_packets += ucast_count;

    return ucast_count + mcast_count;
}

static inline uint32_t _wfd_bulk_skb_get(uint32_t qid, uint32_t budget, wfd_object_t *wfd_p, void **ucast_packets, uint32_t *_ucast_count, 
        void **mcast_packets, uint32_t *_mcast_count)
{
    struct sk_buff *skb;
    rdpa_cpu_rx_info_t info = {};
    wl_metadata_nic_t wl_nic;
    int rc = 0;
    uint32_t ucast_count = 0, mcast_count = 0;

    while (budget)
    {
        rc = rdpa_cpu_packet_get(rdpa_cpu_wlan0 + wfd_p->wl_radio_idx, qid, &info);
        if (unlikely(rc))
            break;

        budget--;
        skb = skb_alloc(&info);

        if (unlikely(!skb))
        {
            gs_count_no_buffers[qid]++;
            continue;
        }
        if (info.is_exception)
        {
            rc = exception_packet_handle(skb, &info, wfd_p);
            if (unlikely(rc == BDMF_ERR_NODEV))
                gs_count_rx_no_wifi_interface[qid]++;
            continue;
        }

#if defined(CC_NBUFF_FLUSH_OPTIMIZATION)
        /* Set dirty pointer to optimize cache invalidate */
        skb_shinfo((struct sk_buff *)(skb))->dirty_p = skb->data + ETH_HLEN;
#endif

        wl_nic.hword = info.wl_metadata;
        if ((skb->wl.ucast.nic.is_ucast = info.is_ucast))
        {
            skb->wl.ucast.nic.wl_chainidx = wl_nic.chain_id;
            ucast_packets[ucast_count++] = skb;
            DECODE_WLAN_PRIORITY_MARK(wl_nic.tx_prio, skb->mark);
        }
        else
        {
            skb->wl.mcast.ssid_vector = wl_nic.ssid_vector;
            mcast_packets[mcast_count++] = skb;
            DECODE_WLAN_PRIORITY_MARK(info.mcast_tx_prio, skb->mark);// this parameter always 0- in MCAST  update priority
        }
    }
    *_ucast_count = ucast_count;
    *_mcast_count = mcast_count;
    return rc;
}

static uint32_t
wfd_bulk_skb_get(unsigned long qid, unsigned long budget, void *priv, void **ucast_packets)
{
    int rc;
    wfd_object_t *wfd_p = (wfd_object_t *)priv;
    uint32_t ucast_count = 0, mcast_count = 0;
    void *mcast_packets[NUM_PACKETS_TO_READ_MAX];

    rc = _wfd_bulk_skb_get(qid, budget, wfd_p, ucast_packets, &ucast_count, mcast_packets, &mcast_count);

    if (rc && rc != BDMF_ERR_NO_MORE)
        printk("%s:%d _wfd_bulk_skb_get failed; rc %d\n", __func__, __LINE__, rc);

    if (mcast_count)
        wfd_mcast_forward(mcast_packets, mcast_count, wfd_p, 0);
       
    if (!ucast_count)
        return mcast_count;

    (void) wfd_p->wfd_fwdHook(ucast_count, (unsigned long)ucast_packets, wfd_p->wl_radio_idx, 0);
    wfd_p->wl_chained_packets += ucast_count;
    wfd_p->count_rx_queue_packets += ucast_count;

    return ucast_count + mcast_count;
}

static int wfd_accelerator_init(void)
{
    return 0;
}

static int wfd_rdpa_init(int wl_radio_idx)
{
    int rc;
    bdmf_object_handle rdpa_cpu_obj, rdpa_port_obj;
    rdpa_port_dp_cfg_t port_cfg = {};
    BDMF_MATTR(cpu_wlan_attrs, rdpa_cpu_drv());
    BDMF_MATTR(rdpa_port_attrs, rdpa_port_drv());

    /* create cpu */
    rdpa_cpu_index_set(cpu_wlan_attrs, rdpa_cpu_wlan0 + wl_radio_idx);
    /* Number of queues for WFD need + 1 for DHD exception traffic */
    rdpa_cpu_num_queues_set(cpu_wlan_attrs, WFD_NUM_QUEUES_PER_WFD_INST + 1);

    if ((rc = bdmf_new_and_set(rdpa_cpu_drv(), NULL, cpu_wlan_attrs, &rdpa_cpu_obj)))
    {
        printk("%s:%s Failed to create cpu wlan%d object rc(%d)\n", __FILE__, __FUNCTION__, rdpa_cpu_wlan0 + wl_radio_idx, rc);
        return -1;
    }

    if ((rc = rdpa_cpu_int_connect_set(rdpa_cpu_obj, true)) && rc != BDMF_ERR_ALREADY)
    {
        printk("%s:%s Failed to connect cpu interrupts rc(%d)\n", __FILE__, __FUNCTION__, rc);
        return -1;
    }

    rdpa_port_index_set(rdpa_port_attrs, rdpa_if_wlan0 + wl_radio_idx);
    rdpa_port_cpu_obj_set(rdpa_port_attrs, rdpa_cpu_obj);
    rc = bdmf_new_and_set(rdpa_port_drv(), NULL, rdpa_port_attrs, &rdpa_port_obj);
    if (rc)
    {
        printk("%s %s Failed to create rdpa port object rc(%d)\n", __FILE__, __FUNCTION__, rc);
        return -1;
    }

    if ((rc = rdpa_port_cfg_get(rdpa_port_obj, &port_cfg)))
    {
        printk("Failed to get configuration for RDPA port %d. rc=%d\n", rdpa_cpu_wlan0 + wl_radio_idx, rc);
        return -1;
    }

    port_cfg.ls_fc_enable = 1; /* flow cache always enable for WLAN ports */
    if ((rc = rdpa_port_cfg_set(rdpa_port_obj, &port_cfg)))
    {
        printk("Failed to set configuration for RDPA port %d. rc=%d\n", rdpa_cpu_wlan0 + wl_radio_idx, rc);
        return -1;
    }

    send_packet_to_upper_layer = wfd_runner_tx;
    send_packet_to_upper_layer_napi = wfd_runner_tx;
    inject_to_fastpath = 1;

    return 0;
}

static void wfd_rdpa_uninit(int wl_radio_idx)
{
    bdmf_object_handle rdpa_cpu_obj, rdpa_port_obj;

    send_packet_to_upper_layer = netif_rx;
    send_packet_to_upper_layer_napi = netif_receive_skb;
    inject_to_fastpath = 0;

    if (!rdpa_port_get(rdpa_if_wlan0 + wl_radio_idx, &rdpa_port_obj))
    {
        bdmf_put(rdpa_port_obj);
        bdmf_destroy(rdpa_port_obj);
    }

    if (!rdpa_cpu_get(rdpa_cpu_wlan0 + wl_radio_idx, &rdpa_cpu_obj))
    {
        bdmf_put(rdpa_cpu_obj);
        bdmf_destroy(rdpa_cpu_obj);
    }
}

static inline int wfd_queue_not_empty(int wl_radio_idx, long qid, int qidx)
{
    return rdpa_cpu_queue_not_empty(rdpa_cpu_wlan0 + wl_radio_idx, qid);
}

static inline void wfd_int_enable(int wl_radio_idx, long qid, int qidx)
{
    rdpa_cpu_int_enable(rdpa_cpu_wlan0 + wl_radio_idx, qid);
}

static inline void wfd_int_disable(int wl_radio_idx, long qid, int qidx)
{
    rdpa_cpu_int_disable(rdpa_cpu_wlan0 + wl_radio_idx, qid);
}

static inline void *wfd_acc_info_get(int wl_radio_idx)
{
    return rdpa_cpu_data_get(rdpa_cpu_wlan0 + wl_radio_idx);
}

static inline int wfd_get_qid(int qidx)
{
    return qidx;
}

static inline int wfd_get_objidx(int qid, int qidx)
{
    return qidx;
}

static int wfd_runner_tx(struct sk_buff *skb)
{ 
    rdpa_cpu_tx_info_t info = {};
    uint32_t hw_port, radio;
    
    hw_port = netdev_path_get_hw_port(skb->dev);
    radio = WLAN_RADIO_GET(hw_port);

    info.port = rdpa_if_wlan0 + radio;
    info.cpu_port = rdpa_cpu_wlan0 + radio;
    info.ssid = WLAN_SSID_GET(hw_port);
    info.method = rdpa_cpu_tx_ingress;

    gs_count_tx_packets[radio]++;
    return rdpa_cpu_send_sysb(skb, &info);
}
#endif /* __XRDP_WFD_INLINE_H_INCLUDED__ */
