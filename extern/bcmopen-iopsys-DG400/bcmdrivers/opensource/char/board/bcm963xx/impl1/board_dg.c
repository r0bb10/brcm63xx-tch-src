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

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include <bcmtypes.h>
#include <bcm_map_part.h>
#include <board.h>
#include <boardparms.h>
#include <bcm_intr.h>
#include <shared_utils.h>
#include <bcm_pinmux.h>

#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148) || defined(CONFIG_BCM963381)
#include "pmc_dsl.h"
#endif

#include "board_dg.h"
#include "board_wl.h"
#include "board_util.h"
#include "board_wd.h"

#if  !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189)
static irqreturn_t kerSysDyingGaspIsr(int irq, void * dev_id);
#endif

/* dgaspMutex Protects dyingGasp enable/disable functions */
/* also protects list add and delete, but is ignored during isr. */
static DEFINE_MUTEX(dgaspMutex);
static volatile int isDyingGaspTriggered = 0;
static CB_DGASP_LIST *g_cb_dgasp_list_head = NULL;

#if !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189)
static int dg_enabled = 0;
static int dg_prevent_enable = 0;
static int dg_active_on_boot = 0;
static int isDGActiveOnBoot(void);
#endif

/***************************************************************************
* Dying gasp ISR and functions.
***************************************************************************/

/* For any driver running on another cpu that needs to know if system is losing
   power */
int kerSysIsDyingGaspTriggered(void)
{
    return isDyingGaspTriggered;
}


#if !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189)
static irqreturn_t kerSysDyingGaspIsr(int irq, void * dev_id)
{
    struct list_head *pos;
    CB_DGASP_LIST *tmp = NULL, *dslCallBack = NULL;
    unsigned short usPassDyingGaspGpio;        // The GPIO pin to propogate a dying gasp signal

    isDyingGaspTriggered = 1;
#if defined(CONFIG_BCM947189)
#else
    UART->Data = 'D';
    UART->Data = '%';
    UART->Data = 'G';
#endif

#if defined (WIRELESS)
    kerSetWirelessPD(WLAN_OFF);
#endif
    /* first to turn off everything other than dsl */
    list_for_each(pos, &g_cb_dgasp_list_head->list) {
        tmp = list_entry(pos, CB_DGASP_LIST, list);
        if(strncmp(tmp->name, "dsl", 3)) {
            (tmp->cb_dgasp_fn)(tmp->context);
        }else {
            dslCallBack = tmp;
        }
    }
    
    // Invoke dying gasp handlers
    if(dslCallBack)
        (dslCallBack->cb_dgasp_fn)(dslCallBack->context);

    /* reset and shutdown system */



    /* Set WD to fire in 1 sec in case power is restored before reset occurs */
#if defined(CONFIG_BCM_WATCHDOG_TIMER)
    bcm_suspend_watchdog();
#endif
    start_watchdog(1000000, 1);

    // If configured, propogate dying gasp to other processors on the board
    if(BpGetPassDyingGaspGpio(&usPassDyingGaspGpio) == BP_SUCCESS)
    {
        // Dying gasp configured - set GPIO
        kerSysSetGpioState(usPassDyingGaspGpio, kGpioInactive);
    }

    // If power is going down, nothing should continue!
    while (1);
    return( IRQ_HANDLED );
}
#endif /* !defined(CONFIG_BCM963138) && !defined(CONFIG_BCM963148)*/


#if !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189)
void kerSysDisableDyingGaspInterrupt( void )
{
    mutex_lock(&dgaspMutex);

    if (!dg_enabled) {
        mutex_unlock(&dgaspMutex);
        return;
    }

    BcmHalInterruptDisable(INTERRUPT_ID_DG);
    printk("DYING GASP IRQ Disabled\n");
    dg_enabled = 0;
    mutex_unlock(&dgaspMutex);
}
EXPORT_SYMBOL(kerSysDisableDyingGaspInterrupt);

static int isDGActiveOnBoot(void)
{
    int dg_active = 0;

    /* Check if DG is already active */
#if defined(CONFIG_BCM96838) || defined(CONFIG_BCM96848) || defined(CONFIG_BCM96858) || defined(CONFIG_BCM94908) || defined(CONFIG_BCM96836)
    dg_active = 0;
#elif defined(CONFIG_BCM963158)
    //TODO158
#elif defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148)
    dg_active =  PERF->IrqStatus[0] & (1 << (INTERRUPT_ID_DG - ISR_TABLE_OFFSET));
#elif defined(CONFIG_BCM963381)
    dg_active =  (PERF->IrqStatus & (((uint64)1) << (INTERRUPT_ID_DG - INTERNAL_ISR_TABLE_OFFSET)))? 1 : 0;
#else
    dg_active =  PERF->IrqControl[0].IrqStatus & (1 << (INTERRUPT_ID_DG - INTERNAL_ISR_TABLE_OFFSET));
#endif

    return dg_active;
}

void kerSysEnableDyingGaspInterrupt( void )
{
    static int dg_mapped = 0;

    mutex_lock(&dgaspMutex);
    
    /* Ignore requests to enable DG if it is already enabled */
    if (dg_enabled) {
        printk("DYING GASP IRQ Already Enabled\n");
        mutex_unlock(&dgaspMutex);
        return;
    }

    /* Set DG Parameters */
#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148) || defined(CONFIG_BCM963381)
    msleep(5);
    /* Setup dying gasp threshold @ 1.25V with 0mV Heysteresis */
    DSLPHY_AFE->BgBiasReg[0] = (DSLPHY_AFE->BgBiasReg[0] & ~0xffff) | 0x04cd;
    /* Note that these settings are based on the ATE characterization of the threshold and hysterises 
     * register settings and as such dont match what is stated in the register descriptions */
    DSLPHY_AFE->AfeReg[0] = (DSLPHY_AFE->AfeReg[0] & ~0xffff) | 0x00EF;
    msleep(5);
#endif /* defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148) || defined(CONFIG_BCM963381) */

    if (dg_prevent_enable) 
    {
        printk("DYING GASP enabling postponed\n");
    } 
    else 
    {        
        if (dg_mapped) 
        { 
            BcmHalInterruptEnable(INTERRUPT_ID_DG);
            printk("DYING GASP IRQ Enabled\n");
        }
        else 
        {
            BcmHalMapInterrupt((FN_HANDLER)kerSysDyingGaspIsr, (void*)0, INTERRUPT_ID_DG);
#if !defined(CONFIG_ARM) && !defined(CONFIG_ARM64)
            BcmHalInterruptEnable( INTERRUPT_ID_DG );
#endif
            dg_mapped = 1;
            printk("DYING GASP IRQ Initialized and Enabled\n");
        }
        dg_enabled = 1;
    }
    mutex_unlock(&dgaspMutex);
}
EXPORT_SYMBOL(kerSysEnableDyingGaspInterrupt);
#endif /* !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189) */

void __init kerSysInitDyingGaspHandler( void )
{
    CB_DGASP_LIST *new_node;

    if( g_cb_dgasp_list_head != NULL) {
        printk("Error: kerSysInitDyingGaspHandler: list head is not null\n");
        return;
    }
    new_node= (CB_DGASP_LIST *)kmalloc(sizeof(CB_DGASP_LIST), GFP_KERNEL);
    memset(new_node, 0x00, sizeof(CB_DGASP_LIST));
    INIT_LIST_HEAD(&new_node->list);
    g_cb_dgasp_list_head = new_node;

#if !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189)
    /* Disable DG Interrupt */
    kerSysDisableDyingGaspInterrupt();

#if defined(CONFIG_BCM96838)
    GPIO->dg_control |= (1 << DG_EN_SHIFT);    
#elif defined(CONFIG_BCM96848)
    MISC_REG->DgSensePadCtrl |= (1 << DG_EN_SHIFT); 
#elif defined(CONFIG_BCM94908) || defined(CONFIG_BCM96858) || defined(CONFIG_BCM96836)
    MISC->miscDgSensePadCtrl |= (1 << DG_EN_SHIFT);
#elif defined(CONFIG_BCM963158)
    //TODO158
#else
    {
#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148) || defined(CONFIG_BCM963381)
        pmc_dsl_power_up();
        pmc_dsl_core_reset();
#endif
    }
#endif /* defined(CONFIG_BCM96838) */

    /* Set DG related global variables */

    mutex_lock(&dgaspMutex);
    /* If DG is currently active, we are assuming it is tied low on purpose */
    dg_active_on_boot = isDGActiveOnBoot();

    /* Prevent DG enable if already active OR this is a battery enabled system */
    dg_prevent_enable = (dg_active_on_boot || kerSysIsBatteryEnabled()); 
    mutex_unlock(&dgaspMutex);

    /* Enable DG Interrupt */
    kerSysEnableDyingGaspInterrupt();
#endif /* !defined(CONFIG_BCM960333) */
} 

void __exit kerSysDeinitDyingGaspHandler( void )
{
    struct list_head *pos;
    CB_DGASP_LIST *tmp;

    if(g_cb_dgasp_list_head == NULL)
        return;

    list_for_each(pos, &g_cb_dgasp_list_head->list) {
        tmp = list_entry(pos, CB_DGASP_LIST, list);
        list_del(pos);
        kfree(tmp);
    }

    kfree(g_cb_dgasp_list_head);
    g_cb_dgasp_list_head = NULL;

} /* kerSysDeinitDyingGaspHandler */

void kerSysRegisterDyingGaspHandler(char *devname, void *cbfn, void *context)
{
    CB_DGASP_LIST *new_node;

    // do all the stuff that can be done without the lock first
    if( devname == NULL || cbfn == NULL ) {
        printk("Error: kerSysRegisterDyingGaspHandler: register info not enough (%s,%p,%p)\n", devname, cbfn, context);
        return;
    }

    if (strlen(devname) > (IFNAMSIZ - 1)) {
        printk("Warning: kerSysRegisterDyingGaspHandler: devname too long, will be truncated\n");
    }

    new_node= (CB_DGASP_LIST *)kmalloc(sizeof(CB_DGASP_LIST), GFP_KERNEL);
    memset(new_node, 0x00, sizeof(CB_DGASP_LIST));
    INIT_LIST_HEAD(&new_node->list);
    strncpy(new_node->name, devname, IFNAMSIZ-1);
    new_node->cb_dgasp_fn = (cb_dgasp_t)cbfn;
    new_node->context = context;

    // OK, now acquire the lock and insert into list
    mutex_lock(&dgaspMutex);
    if( g_cb_dgasp_list_head == NULL) {
        printk("Error: kerSysRegisterDyingGaspHandler: list head is null\n");
        kfree(new_node);
    } else {
        list_add(&new_node->list, &g_cb_dgasp_list_head->list);
        printk("dgasp: kerSysRegisterDyingGaspHandler: %s registered \n", devname);
    }
    mutex_unlock(&dgaspMutex);

    return;
} /* kerSysRegisterDyingGaspHandler */

void kerSysDeregisterDyingGaspHandler(char *devname)
{
    struct list_head *pos;
    CB_DGASP_LIST *tmp;
    int found=0;

    if(devname == NULL) {
        printk("Error: kerSysDeregisterDyingGaspHandler: devname is null\n");
        return;
    }

    printk("kerSysDeregisterDyingGaspHandler: %s is deregistering\n", devname);

    mutex_lock(&dgaspMutex);
    if(g_cb_dgasp_list_head == NULL) {
        printk("Error: kerSysDeregisterDyingGaspHandler: list head is null\n");
    } else {
        list_for_each(pos, &g_cb_dgasp_list_head->list) {
            tmp = list_entry(pos, CB_DGASP_LIST, list);
            if(!strcmp(tmp->name, devname)) {
                list_del(pos);
                kfree(tmp);
                found = 1;
                printk("kerSysDeregisterDyingGaspHandler: %s is deregistered\n", devname);
                break;
            }
        }
        if (!found)
            printk("kerSysDeregisterDyingGaspHandler: %s not (de)registered\n", devname);
    }
    mutex_unlock(&dgaspMutex);

    return;
} /* kerSysDeregisterDyingGaspHandler */

#if !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189)
void kerSysDyingGaspIoctl(DGASP_ENABLE_OPTS opt)
{
    switch (opt)
    {
        case DG_ENABLE_FORCE:
            mutex_lock(&dgaspMutex);
            if( !dg_active_on_boot )
                dg_prevent_enable = 0;
            mutex_unlock(&dgaspMutex);
            /* FALLTHROUGH */
        case DG_ENABLE:
             kerSysEnableDyingGaspInterrupt();
             break;
        case DG_DISABLE_PREVENT_ENABLE:
             mutex_lock(&dgaspMutex);
             dg_prevent_enable = 1;
             mutex_unlock(&dgaspMutex);
             /* FALLTHROUGH */
        case DG_DISABLE:
             kerSysDisableDyingGaspInterrupt();
             break;
        default:
             break;
    }

    return;
}
#endif
