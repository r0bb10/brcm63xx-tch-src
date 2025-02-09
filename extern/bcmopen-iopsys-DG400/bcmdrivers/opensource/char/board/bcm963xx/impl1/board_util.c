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

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <bcmnetlink.h>
#include <net/sock.h>

#include <bcmtypes.h>
#include <bcm_map_part.h>
#include <board.h>
#include <boardparms.h>
#include <boardparms_voice.h>
#include <flash_api.h>
#include <flash_common.h>
#include <shared_utils.h>
#include <bcm_pinmux.h>
#include <bcmpci.h>
#include <linux/bcm_log.h>
#include <bcmSpiRes.h>
#if defined(CONFIG_BCM_6802_MoCA)
#include "./moca/board_moca.h"
#include "./bbsi/bbsi.h"
#else
#include <spidevices.h>
#endif
#include "bcm_otp.h"

#include "board_util.h"
#include "board_image.h"
#include "board_dg.h"
#include "board_wd.h"

#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148)
#if defined(CONFIG_SMP)
#include <linux/cpu.h>
#endif
#include "pmc_dsl.h"
#include "pmc_apm.h"
#endif

#if defined(CONFIG_BCM96838) || defined(CONFIG_BCM94908)
#include "pmc_drv.h"
#include "BPCM.h"
#endif

#if defined(CONFIG_BCM963381)
#include "pmc_dsl.h"
#endif
#include <net/genetlink.h>

extern int g_ledInitialized;
static int gen_nl_enable = 0;

// macAddrMutex is used by kerSysGetMacAddress and kerSysReleaseMacAddress
// to protect access to g_pMacInfo
static PMAC_INFO g_pMacInfo = NULL;
static DEFINE_MUTEX(macAddrMutex);
static PGPON_INFO g_pGponInfo = NULL;
static unsigned long g_ulSdramSize;

#define MAX_PAYLOAD_LEN 64
#define GEN_MSG_MAX_LEN 128
static struct sock *g_monitor_nl_sk;
static int g_monitor_nl_pid = 0 ;

static kerSysMacAddressNotifyHook_t kerSysMacAddressNotifyHook = NULL;

#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148)
/*SATA Test module callback */
int (*bcm_sata_test_ioctl_fn)(void *) = NULL; 
EXPORT_SYMBOL(bcm_sata_test_ioctl_fn);
#endif


/* A global variable used by Power Management and other features to determine if Voice is idle or not */
volatile int isVoiceIdle = 1;
EXPORT_SYMBOL(isVoiceIdle);

BOARD_IOC* board_ioc_alloc(void)
{
    BOARD_IOC *board_ioc =NULL;
    board_ioc = (BOARD_IOC*) kmalloc( sizeof(BOARD_IOC) , GFP_KERNEL );
    if(board_ioc)
    {
        memset(board_ioc, 0, sizeof(BOARD_IOC));
    }
    return board_ioc;
}

void board_ioc_free(BOARD_IOC* board_ioc)
{
    if(board_ioc)
    {
        kfree(board_ioc);
    }
}

int ConfigCs (BOARD_IOCTL_PARMS *parms)
{
    int                     retv = 0;
    return( retv );
}

void print_rst_status(void)
{
#if !defined(CONFIG_BCM96838) && !defined(CONFIG_BCM963268) && !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189)
    unsigned int resetStatus = TIMER->ResetStatus & RESET_STATUS_MASK;
    printk("%s: Last RESET due to ", __FUNCTION__);
    switch ( resetStatus )
    {
       case PCIE_RESET_STATUS:
          printk("PCIE reset\n");
          break;
       case SW_RESET_STATUS:
          printk("SW reset\n");
          break;
       case HW_RESET_STATUS:
          printk("HW reset\n");
          break;
       case POR_RESET_STATUS:
          printk("POR reset\n");
          break;
       default:
          printk("Unknown\n");
    }
#if defined(CONFIG_BCM94908) || defined(CONFIG_BCM96858) || defined(CONFIG_BCM96848) || defined(CONFIG_BCM96836) || defined(CONFIG_BCM963158)
    printk("%s: RESET reason: 0x%08x\n", __FUNCTION__, TIMER->ResetReason);  
#endif
#endif
}

int kerSysIsBatteryEnabled(void)
{
#if defined(CONFIG_BCM_BMU)
    unsigned short bmuen;

    if (BpGetBatteryEnable(&bmuen) == BP_SUCCESS) {
        return (bmuen);
    }
#endif
    return 0;
}

void __init kerSysInitBatteryManagementUnit(void)
{
#if defined(CONFIG_BCM_BMU)
    if (kerSysIsBatteryEnabled()) {
        pmc_apm_power_up();
#if defined(CONFIG_BCM963148)
        // APM_ANALOG_BG_BOOST and APM_LDO_VREGCNTL_7 default to 0 in 63148 and need to be set
        APM_PUB->reg_apm_analog_bg |= APM_ANALOG_BG_BOOST;
        APM_PUB->reg_codec_config_4 |= APM_LDO_VREGCNTL_7;
#endif
    }
#endif
}

PMAC_INFO get_mac_info(void)
{
    return  g_pMacInfo;
}

void __init set_mac_info( void )
{
    NVRAM_DATA *pNvramData;
    unsigned long ulNumMacAddrs;

    if (NULL == (pNvramData = readNvramData()))
    {
        printk("set_mac_info: could not read nvram data\n");
        return;
    }

    ulNumMacAddrs = pNvramData->ulNumMacAddrs;

    if( ulNumMacAddrs > 0 && ulNumMacAddrs <= NVRAM_MAC_COUNT_MAX )
    {
        unsigned long ulMacInfoSize =
            sizeof(MAC_INFO) + ((sizeof(MAC_ADDR_INFO)) * (ulNumMacAddrs-1));

        g_pMacInfo = (PMAC_INFO) kmalloc( ulMacInfoSize, GFP_KERNEL );

        if( g_pMacInfo )
        {
            memset( g_pMacInfo, 0x00, ulMacInfoSize );
            g_pMacInfo->ulNumMacAddrs = pNvramData->ulNumMacAddrs;
            memcpy( g_pMacInfo->ucaBaseMacAddr, pNvramData->ucaBaseMacAddr,
                NVRAM_MAC_ADDRESS_LEN );
        }
        else
            printk("ERROR - Could not allocate memory for MAC data\n");
    }
    else
        printk("ERROR - Invalid number of MAC addresses (%ld) is configured.\n",
        ulNumMacAddrs);
    kfree(pNvramData);
}

static int gponParamsAreErased(NVRAM_DATA *pNvramData)
{
    int i;
    int erased = 1;

    for(i=0; i<NVRAM_GPON_SERIAL_NUMBER_LEN-1; ++i) {
        if((pNvramData->gponSerialNumber[i] != (char) 0xFF) &&
            (pNvramData->gponSerialNumber[i] != (char) 0x00)) {
                erased = 0;
                break;
        }
    }

    if(!erased) {
        for(i=0; i<NVRAM_GPON_PASSWORD_LEN-1; ++i) {
            if((pNvramData->gponPassword[i] != (char) 0xFF) &&
                (pNvramData->gponPassword[i] != (char) 0x00)) {
                    erased = 0;
                    break;
            }
        }
    }

    return erased;
}

void __init set_gpon_info( void )
{
    NVRAM_DATA *pNvramData;

    if (NULL == (pNvramData = readNvramData()))
    {
        printk("set_gpon_info: could not read nvram data\n");
        return;
    }

    g_pGponInfo = (PGPON_INFO) kmalloc( sizeof(GPON_INFO), GFP_KERNEL );

    if( g_pGponInfo )
    {
        if ((pNvramData->ulVersion < NVRAM_FULL_LEN_VERSION_NUMBER) ||
            gponParamsAreErased(pNvramData))
        {
            strcpy( g_pGponInfo->gponSerialNumber, DEFAULT_GPON_SN );
            strcpy( g_pGponInfo->gponPassword, DEFAULT_GPON_PW );
        }
        else
        {
            strncpy( g_pGponInfo->gponSerialNumber, pNvramData->gponSerialNumber,
                NVRAM_GPON_SERIAL_NUMBER_LEN );
            g_pGponInfo->gponSerialNumber[NVRAM_GPON_SERIAL_NUMBER_LEN-1]='\0';
            strncpy( g_pGponInfo->gponPassword, pNvramData->gponPassword,
                NVRAM_GPON_PASSWORD_LEN );
            g_pGponInfo->gponPassword[NVRAM_GPON_PASSWORD_LEN-1]='\0';
        }
    }
    else
    {
        printk("ERROR - Could not allocate memory for GPON data\n");
    }
    kfree(pNvramData);
}

//**************************************************************************************
// Utitlities for dump memory, free kernel pages, mips soft reset, etc.
//**************************************************************************************

/***********************************************************************
* Function Name: dumpaddr
* Description  : Display a hex dump of the specified address.
***********************************************************************/
void dumpaddr( unsigned char *pAddr, int nLen )
{
    static char szHexChars[] = "0123456789abcdef";
    char szLine[80];
    char *p = szLine;
    unsigned char ch, *q;
    int i, j;
    unsigned int ul;

    while( nLen > 0 )
    {
        sprintf( szLine, "%8.8lx: ", (unsigned long) pAddr );
        p = szLine + strlen(szLine);

        for(i = 0; i < 16 && nLen > 0; i += sizeof(int), nLen -= sizeof(int))
        {
            ul = *(unsigned int *) &pAddr[i];
            q = (unsigned char *) &ul;
            for( j = 0; j < sizeof(int); j++ )
            {
                *p++ = szHexChars[q[j] >> 4];
                *p++ = szHexChars[q[j] & 0x0f];
                *p++ = ' ';
            }
        }

        for( j = 0; j < 16 - i; j++ )
            *p++ = ' ', *p++ = ' ', *p++ = ' ';

        *p++ = ' ', *p++ = ' ', *p++ = ' ';

        for( j = 0; j < i; j++ )
        {
            ch = pAddr[j];
            *p++ = (ch > ' ' && ch < '~') ? ch : '.';
        }

        *p++ = '\0';
        printk( "%s\r\n", szLine );

        pAddr += i;
    }
    printk( "\r\n" );
} /* dumpaddr */



/* This is the low level hardware reset function. Normally called from kernel_restart,
 * the generic linux way of rebooting, which calls a notifier list, stop other cpu, disable 
 * local irq and lets sub-systems know that system is rebooting, and then calls machine_restart, 
 * which eventually call kerSysSoftReset. Do not call this function directly.
 */
void kerSysSoftReset(void)
{
    unsigned long cpu;
    cpu = smp_processor_id();
    printk(KERN_INFO "kerSysSoftReset: called on cpu %lu\n", cpu);
    // FIXME - Once in many thousands of reboots, this function could 
    // fail to complete a reboot.  Arm the watchdog just in case.

    // give enough time(25ms) for resetPwrmgmtDdrMips and other function to finish
    // 4908 need to access some BPCM register which takes very long time to read/write
    start_watchdog(25000, 1);

    resetPwrmgmtDdrMips();
}

extern void stop_other_cpu(void);  // in arch/mips/kernel/smp.c

/* this function is only called in SPI nor kernel flash update */
void stopOtherCpu(void)
{
#if defined(CONFIG_BCM96858) || defined(CONFIG_BCM94908) || defined(CONFIG_BCM96836) || defined(CONFIG_BCM963158)
#if defined(CONFIG_SMP)
    /* This cause call stack dump unless we hack the system_state to reboot. See
       ipi_cpu_stop() in arch/arm64/kernel/smp.c. But not a big deal as this function
       is only used in SPI nor which is not offically supported in 63158/4908/6858 */
    smp_send_stop();
#endif

#elif defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148)
#if defined(CONFIG_SMP)
    /* in ARM, CPU#0 should be the last one to get shut down, and for
     * both 63138 and 63148, we have dualcore system, so we can hardcode
     * cpu_down() on CPU#1. Also, if this function is handled by the 
     * CPU which is going to be shut down, kernel will transfer the
     * current task to another CPU.  Thus when we return from cpu_down(),
     * the task is still running. */
    cpu_down(1);
#endif
#elif defined(CONFIG_BCM947189)
#else
#if defined(CONFIG_SMP)
    stop_other_cpu();
#endif
#endif /* !defined(CONFIG_BCM963138) && !defined(CONFIG_BCM963148) */
}

void resetPwrmgmtDdrMips(void)
{
#if defined(CONFIG_BCM96838)
    WDTIMER->WD0DefCount = 0;
    WDTIMER->WD0Ctl = 0xee00;
    WDTIMER->WD0Ctl = 0x00ee;
    WDTIMER->WD1DefCount = 0;
    WDTIMER->WD1Ctl = 0xee00;
    WDTIMER->WD1Ctl = 0x00ee;
    PERF->TimerControl |= SOFT_RESET_0;
#else
#if defined (CONFIG_BCM963268)
    MISC->miscVdslControl &= ~(MISC_VDSL_CONTROL_VDSL_MIPS_RESET | MISC_VDSL_CONTROL_VDSL_MIPS_POR_RESET );

    // Power Management on Ethernet Ports may have brought down EPHY PLL
    // and soft reset below will lock-up 6362 if the PLL is not up
    // therefore bring it up here to give it time to stabilize
    GPIO->RoboswEphyCtrl &= ~EPHY_PWR_DOWN_DLL;
#endif
#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148)
    /* stop SF2 switch from sending packet to runner, or the DMA might get stuck.
     * Also give it time to complete the ongoing DMA transaction. */
    ETHSW_CORE->imp_port_state &= ~ETHSW_IPS_LINK_PASS;
#endif

#if defined(CONFIG_BCM94908)
    /* reset the pll manually to bypass mode if strap for slow clock */
    if (MISC->miscStrapBus&MISC_STRAP_BUS_CPU_SLOW_FREQ)
    {
        PLL_CTRL_REG ctrl_reg;
        ReadBPCMRegister(PMB_ADDR_B53PLL, PLLBPCMRegOffset(resets), &ctrl_reg.Reg32);
        ctrl_reg.Bits.byp_wait = 1;
        WriteBPCMRegister(PMB_ADDR_B53PLL, PLLBPCMRegOffset(resets), ctrl_reg.Reg32);
    }
#endif

    // let UART finish printing
    udelay(100);

#if defined(CONFIG_BCM963268)
    PERF->pll_control |= SOFT_RESET;    // soft reset mips
#elif defined(CONFIG_BCM960333)
    /*
     * After a soft-reset, one of the reserved bits of TIMER->SoftRst remains
     * enabled and the next soft-reset won't work unless TIMER->SoftRst is
     * set to 0.
     */
    TIMER->SoftRst = 0;
    TIMER->SoftRst |= SOFT_RESET;
#elif defined(CONFIG_BCM947189)
    GPIO_WATCHDOG->watchdog = 1;
#elif (CONFIG_BCM_CHIP_REV == 0x6858B0) || defined(CONFIG_BCM963158)
    WDTIMER0->SoftRst = 1;
#else
    TIMER->SoftRst = 1;
#endif

#endif
    for(;;) {} // spin mips and wait soft reset to take effect
}

unsigned long kerSysGetMacAddressType( unsigned char *ifName )
{
    unsigned long macAddressType = MAC_ADDRESS_ANY;

    if(strstr(ifName, IF_NAME_ETH))
    {
        macAddressType = MAC_ADDRESS_ETH;
    }
    else if(strstr(ifName, IF_NAME_USB))
    {
        macAddressType = MAC_ADDRESS_USB;
    }
    else if(strstr(ifName, IF_NAME_WLAN))
    {
        macAddressType = MAC_ADDRESS_WLAN;
    }
    else if(strstr(ifName, IF_NAME_MOCA))
    {
        macAddressType = MAC_ADDRESS_MOCA;
    }
    else if(strstr(ifName, IF_NAME_ATM))
    {
        macAddressType = MAC_ADDRESS_ATM;
    }
    else if(strstr(ifName, IF_NAME_PTM))
    {
        macAddressType = MAC_ADDRESS_PTM;
    }
    else if(strstr(ifName, IF_NAME_GPON) || strstr(ifName, IF_NAME_VEIP))
    {
        macAddressType = MAC_ADDRESS_GPON;
    }
    else if(strstr(ifName, IF_NAME_EPON))
    {
        macAddressType = MAC_ADDRESS_EPON;
    }

    return macAddressType;
}

static inline void kerSysMacAddressNotify(unsigned char *pucaMacAddr, MAC_ADDRESS_OPERATION op)
{
    if(kerSysMacAddressNotifyHook)
    {
        kerSysMacAddressNotifyHook(pucaMacAddr, op);
    }
}

int kerSysMacAddressNotifyBind(kerSysMacAddressNotifyHook_t hook)
{
    int nRet = 0;

    if(hook && kerSysMacAddressNotifyHook)
    {
        printk("ERROR: kerSysMacAddressNotifyHook already registered! <0x%p>\n",
               kerSysMacAddressNotifyHook);
        nRet = -EINVAL;
    }
    else
    {
        kerSysMacAddressNotifyHook = hook;
    }

    return nRet;
}

static void getNthMacAddr( unsigned char *pucaMacAddr, unsigned long n)
{
    unsigned long macsequence = 0;
    /* Work with only least significant three bytes of base MAC address */
    macsequence = (pucaMacAddr[3] << 16) | (pucaMacAddr[4] << 8) | pucaMacAddr[5];
    macsequence = (macsequence + n) & 0xffffff;
    pucaMacAddr[3] = (macsequence >> 16) & 0xff;
    pucaMacAddr[4] = (macsequence >> 8) & 0xff;
    pucaMacAddr[5] = (macsequence ) & 0xff;

}
static unsigned long getIdxForNthMacAddr( const unsigned char *pucaBaseMacAddr, unsigned char *pucaMacAddr)
{
    unsigned long macSequence = 0;
    unsigned long baseMacSequence = 0;
    
    macSequence = (pucaMacAddr[3] << 16) | (pucaMacAddr[4] << 8) | pucaMacAddr[5];
    baseMacSequence = (pucaBaseMacAddr[3] << 16) | (pucaBaseMacAddr[4] << 8) | pucaBaseMacAddr[5];

    return macSequence - baseMacSequence;
}
/* Allocates requested number of consecutive MAC addresses */
int kerSysGetMacAddresses( unsigned char *pucaMacAddr, unsigned int num_addresses, unsigned long ulId )
{
    int nRet = -EADDRNOTAVAIL;
    PMAC_ADDR_INFO pMai = NULL;
    PMAC_ADDR_INFO pMaiFreeId = NULL, pMaiFreeIdTemp;
    unsigned long i = 0, j = 0, ulIdxId = 0;

    mutex_lock(&macAddrMutex);

    /* Start with the base address */
    memcpy( pucaMacAddr, g_pMacInfo->ucaBaseMacAddr, NVRAM_MAC_ADDRESS_LEN);

#if defined(CONFIG_BCM96838) || defined(CONFIG_BCM96848) || defined(CONFIG_BCM96858) || defined(CONFIG_BCM96836)
    /*As epon mac should not be dynamicly changed, always use last 1(SLLID) or 8(MLLID) mac address(es)*/
    if (ulId == MAC_ADDRESS_EPONONU)
    {
        i = g_pMacInfo->ulNumMacAddrs - num_addresses; 

        for (j = 0, pMai = &g_pMacInfo->MacAddrs[i]; j < num_addresses; j++) {
            pMaiFreeIdTemp = pMai + j;
            if (pMaiFreeIdTemp->chInUse != 0 && pMaiFreeIdTemp->ulId != MAC_ADDRESS_EPONONU) {
                printk("kerSysGetMacAddresses: epon mac address allocate failed, g_pMacInfo[%ld] reserved by 0x%lx\n", i+j, pMaiFreeIdTemp->ulId);    
                break;
            }
        }
        
        if (j >= num_addresses) {
            pMaiFreeId = pMai;
            ulIdxId = i;
        }
    }
    else
#endif    
    {
        for( i = 0, pMai = g_pMacInfo->MacAddrs; i < g_pMacInfo->ulNumMacAddrs;
            i++, pMai++ )
        {
            if( ulId == pMai->ulId || ulId == MAC_ADDRESS_ANY )
            {
                /* This MAC address has been used by the caller in the past. */
                getNthMacAddr( pucaMacAddr, i );
                pMai->chInUse = 1;
                pMaiFreeId = NULL;
                nRet = 0;
                break;
            } else if( pMai->chInUse == 0 ) {
                /* check if it there are num_addresses to be checked starting from found MAC address */
                if ((i + num_addresses - 1) >= g_pMacInfo->ulNumMacAddrs) {
                    break;
                }
    
                for (j = 1; j < num_addresses; j++) {
                    pMaiFreeIdTemp = pMai + j;
                    if (pMaiFreeIdTemp->chInUse != 0) {
                        break;
                    }
                }
                if (j == num_addresses) {
                    pMaiFreeId = pMai;
                    ulIdxId = i;
                    break;
                }
            }
        }
    }

    if(pMaiFreeId )
    {
        /* An available MAC address was found. */
        getNthMacAddr( pucaMacAddr, ulIdxId );
        pMaiFreeIdTemp = pMai;
        for (j = 0; j < num_addresses; j++) {
            pMaiFreeIdTemp->ulId = ulId;
            pMaiFreeIdTemp->chInUse = 1;
            pMaiFreeIdTemp++;
        }
        nRet = 0;
    } else if (nRet) {
        /* No address found, return with last address from range */
        getNthMacAddr( pucaMacAddr, g_pMacInfo->ulNumMacAddrs - 1 );
    }

    mutex_unlock(&macAddrMutex);

    return( nRet );
} /* kerSysGetMacAddr */
int kerSysGetMacAddress( unsigned char *pucaMacAddr, unsigned long ulId )
{
    return kerSysGetMacAddresses(pucaMacAddr,1,ulId); /* Get only one address */
} /* kerSysGetMacAddr */


int kerSysReleaseMacAddresses( unsigned char *pucaMacAddr, unsigned int num_addresses )
{
    int i, nRet = -EINVAL;
    unsigned long ulIdx = 0;

    mutex_lock(&macAddrMutex);

    ulIdx = getIdxForNthMacAddr(g_pMacInfo->ucaBaseMacAddr, pucaMacAddr);

    if( ulIdx < g_pMacInfo->ulNumMacAddrs )
    {
        for(i=0; i<num_addresses; i++) {
            if ((ulIdx + i) < g_pMacInfo->ulNumMacAddrs) {
                PMAC_ADDR_INFO pMai = &g_pMacInfo->MacAddrs[ulIdx + i];
                if( pMai->chInUse == 1 )
                {
                    pMai->chInUse = 0;
                    nRet = 0;
                }
            } else {
                printk("Request to release %d addresses failed as "
                    " the one or more of the addresses, starting from"
                    " %dth address from given address, requested for release"
                    " is not in the list of available MAC addresses \n", num_addresses, i);
                break;
            }
        }
    }

    mutex_unlock(&macAddrMutex);

    return( nRet );
} /* kerSysReleaseMacAddr */


int kerSysReleaseMacAddress( unsigned char *pucaMacAddr )
{
    return kerSysReleaseMacAddresses(pucaMacAddr,1); /* Release only one MAC address */

} /* kerSysReleaseMacAddr */


void kerSysGetGponSerialNumber( unsigned char *pGponSerialNumber )
{
    strcpy( pGponSerialNumber, g_pGponInfo->gponSerialNumber );
}


void kerSysGetGponPassword( unsigned char *pGponPassword )
{
    strcpy( pGponPassword, g_pGponInfo->gponPassword );
}

int kerSysGetSdramSize( void )
{
    return( (int) g_ulSdramSize );
} /* kerSysGetSdramSize */
int kerSysGetAfeId( unsigned int *afeId )
{
    NVRAM_DATA *pNvramData;

    if (NULL == (pNvramData = readNvramData()))
    {
        printk("kerSysGetAfeId: could not read nvram data\n");
        return -1;
    }

    afeId [0] = pNvramData->afeId[0];
    afeId [1] = pNvramData->afeId[1];
    kfree(pNvramData);

    return 0;
}

int kerSysGetHwVer(char *HwVer) {
    NVRAM_DATA *pNvramData;

    if (NULL == (pNvramData = readNvramData()))
    {
        printk("kerSysGetHwVer: could not read nvram data\n");
        return -1;
    }

    memcpy( HwVer, pNvramData->hwVer, NVRAM_HWVER_LEN);
    kfree(pNvramData);
    return 0;
}

void kerSysLedCtrl(BOARD_LED_NAME ledName, BOARD_LED_STATE ledState)
{
    if (g_ledInitialized)
       boardLedCtrl(ledName, ledState);
}

/*functionto receive message from usersapce
 * Currently we dont expect any messages fromm userspace
 */
void kerSysRecvFrmMonitorTask(struct sk_buff *skb)
{

   /*process the message here*/
   printk(KERN_WARNING "unexpected skb received at %s \n",__FUNCTION__);
   kfree_skb(skb);
   return;
}

void kerSysInitMonitorSocket( void )
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0)
   g_monitor_nl_sk = netlink_kernel_create(&init_net, NETLINK_BRCM_MONITOR, 0, kerSysRecvFrmMonitorTask, NULL, THIS_MODULE);
#else
   struct netlink_kernel_cfg cfg = {
       .input  = kerSysRecvFrmMonitorTask,
   };
   g_monitor_nl_sk = netlink_kernel_create(&init_net, NETLINK_BRCM_MONITOR, &cfg);
#endif

   if(!g_monitor_nl_sk)
   {
      printk(KERN_ERR "Failed to create a netlink socket for monitor\n");
      return;
   }

}

void kerSysSetMonitorTaskPid(int pid)
{
    g_monitor_nl_pid = pid;
    return;
}

void kerSysSendtoMonitorTask(int msgType, char *msgData, int msgDataLen)
{

   struct sk_buff *skb =  NULL;
   struct nlmsghdr *nl_msgHdr = NULL;
   unsigned int nl_msgLen;

   if(msgData && (msgDataLen > MAX_PAYLOAD_LEN))
   {
      printk(KERN_ERR "invalid message len in %s",__FUNCTION__);
      return;
   } 
	
   nl_msgLen = NLMSG_SPACE(msgDataLen);

   /*Alloc skb ,this check helps to call the fucntion from interrupt context */

   if(in_atomic())
   {
      skb = alloc_skb(nl_msgLen, GFP_ATOMIC);
   }
   else
   {
      skb = alloc_skb(nl_msgLen, GFP_KERNEL);
   }

   if(!skb)
   {
      printk(KERN_ERR "failed to alloc skb in %s",__FUNCTION__);
      return;
   }

   nl_msgHdr = (struct nlmsghdr *)skb->data;
   nl_msgHdr->nlmsg_type = msgType;
   nl_msgHdr->nlmsg_pid=0;/*from kernel */
   nl_msgHdr->nlmsg_len = nl_msgLen;
   nl_msgHdr->nlmsg_flags =0;

   if(msgData)
   {
      memcpy(NLMSG_DATA(nl_msgHdr),msgData,msgDataLen);
   }

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0)
   NETLINK_CB(skb).pid = 0; /*from kernel */
   NETLINK_CB(skb).dst_group = 1;
#endif

   skb->len = nl_msgLen; 

   //changed from unicast to broadcast to be injected to brcmhotproxy  
   netlink_broadcast(g_monitor_nl_sk, skb, 0, 1,GFP_KERNEL);
   return;
}

void __exit kerSysCleanupMonitorSocket(void)
{
   g_monitor_nl_pid = 0 ;
   sock_release(g_monitor_nl_sk->sk_socket);
}

int kerSysGetSerialnum( char *serialnumber )
{
    NVRAM_DATA *pNvramData;

    if (NULL == (pNvramData = readNvramData()))
    {
        printk("kerSysGetSerialnum: could not read nvram data\n");
        return -1;
    }
    strncpy(serialnumber, pNvramData->szSerialNumber, sizeof(pNvramData->szSerialNumber));
    kfree(pNvramData);

    return 0;
}

int kerSysGetBaseMacAddr( unsigned char *basemacaddr )
{
    NVRAM_DATA *pNvramData;
    int i = 0;

    if (NULL == (pNvramData = readNvramData()))
    {
        printk("kerSysGetBaseMacAddr: could not read nvram data\n");
        return -1;
    }

   for ( i=0; i<sizeof(pNvramData->ucaBaseMacAddr); i++)
		basemacaddr[i] = pNvramData->ucaBaseMacAddr[i];
   kfree(pNvramData);
    return 0;
}

EXPORT_SYMBOL(kerSysGetSerialnum);
EXPORT_SYMBOL(kerSysGetBaseMacAddr);
EXPORT_SYMBOL(kerSysGetImageVersion);


/*======================
generic netlink module start
*/
	
static const struct genl_multicast_group inteno_event_mcgrps[] = {
		 { .name = "notify", },
};

static struct genl_family inteno_nl_gnl_family = {
		 .id = GENL_ID_GENERATE,
		 .name = "Inteno",
		 .version = 1,
		 .maxattr = INTENO_NL_MAX,
		 .mcgrps = inteno_event_mcgrps,
		 .n_mcgrps = ARRAY_SIZE(inteno_event_mcgrps),
};

void inteno_netlink_init(void)
{
	int rc;

	rc = genl_register_family(&inteno_nl_gnl_family);
	printk("Inteno netlink init\n");
	if (rc != 0 )
		printk(KERN_ERR "%s: genl_register_family() returned error\n", __FUNCTION__);
	gen_nl_enable = 1;
}

void __exit inteno_netlink_exit(void)
{
	if(gen_nl_enable)
		genl_unregister_family(&inteno_nl_gnl_family);
}

void inteno_netlink_string(const char *s)
{
	struct sk_buff *skb;
	int rc;
	void *msg_head;
	static int seq;
	static char msg_buf[GEN_MSG_MAX_LEN];
		
	if(!gen_nl_enable)
		return ;
		
	skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (skb == NULL)
		return;

		 /* create the message headers */
	msg_head = genlmsg_put(skb, 0, seq++, &inteno_nl_gnl_family, 0, INTENO_NL_MSG);
	if (msg_head == NULL) {
		rc = -ENOMEM;
		printk(KERN_ERR "genlmsg_put() failed\n");
		goto failure;
	}

		 /* link up or down */
	if(s)
		snprintf(msg_buf, GEN_MSG_MAX_LEN, "%s", s);
	/* add a DOC_EXMPL_A_MSG attribute */
	rc = nla_put_string(skb, INTENO_NL_MSG, msg_buf);
	if (rc != 0){
		printk(KERN_ERR "nla_put_string() failed\n");
		goto failure;
	}

		 /* finalize the message */
	genlmsg_end(skb, msg_head);

	rc = genlmsg_multicast_allns(&inteno_nl_gnl_family, skb, 0, 0, GFP_ATOMIC);
	if (rc != 0) {
		printk(KERN_ERR "genlmsg_multicast() failed = %d\n",rc);
		goto failure;
	}

failure:
		 return;
}

void inteno_netlink_linkscan(char *port_name, int state)
{
	static char msg_buf[GEN_MSG_MAX_LEN];

	/* link up or down */
	if (state)
		snprintf(msg_buf, GEN_MSG_MAX_LEN, "switch '{ \"port\" : \"%s\", \"link\" : \"up\" }'", port_name);
	else
		snprintf(msg_buf, GEN_MSG_MAX_LEN, "switch '{ \"port\" : \"%s\", \"link\" : \"down\" }'", port_name);

	inteno_netlink_string(msg_buf);

	return;
}

#define DSL_MSG_TMPL	  "dsl '{%s}'"
void inteno_netlink_dsl(char * msg)
{
	static char msg_buf[GEN_MSG_MAX_LEN];
	snprintf(msg_buf, GEN_MSG_MAX_LEN, DSL_MSG_TMPL, msg);
	inteno_netlink_string(msg_buf);

	return;
}
/*-----------------------------
generic netlink module end
*/

/***********************************************************************
 * Function Name: kerSysBlParmsGetInt
 * Description  : Returns the integer value for the requested name from
 *                the boot loader parameter buffer.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
int kerSysBlParmsGetInt( char *name, int *pvalue )
{
    char *p2, *p1 = (char*)bcm_get_blparms();
    int ret = -1;

    *pvalue = -1;

    /* The g_blparms_buf buffer contains one or more contiguous NULL termianted
     * strings that ends with an empty string.
     */
    while( *p1 )
    {
        p2 = p1;

        while( *p2 != '=' && *p2 != '\0' )
            p2++;

        if( *p2 == '=' )
        {
            *p2 = '\0';

            if( !strcmp(p1, name) )
            {
                *p2++ = '=';
                *pvalue = simple_strtol(p2, &p1, 0);
                if( *p1 == '\0' )
                    ret = 0;
                break;
            }

            *p2 = '=';
        }

        p1 += strlen(p1) + 1;
    }

    return( ret );
}

/***********************************************************************
 * Function Name: kerSysBlParmsGetStr
 * Description  : Returns the string value for the requested name from
 *                the boot loader parameter buffer.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
int kerSysBlParmsGetStr( char *name, char *pvalue, int size )
{
    char *p2, *p1 = (char*)bcm_get_blparms();
    int ret = -1;

    /* The g_blparms_buf buffer contains one or more contiguous NULL termianted
     * strings that ends with an empty string.
     */
    while( *p1 )
    {
        p2 = p1;

        while( *p2 != '=' && *p2 != '\0' )
            p2++;

        if( *p2 == '=' )
        {
            *p2 = '\0';

            if( !strcmp(p1, name) )
            {
                *p2++ = '=';
                strncpy(pvalue, p2, size);
                ret = 0;
                break;
            }

            *p2 = '=';
        }

        p1 += strlen(p1) + 1;
    }

    return( ret );
}


static DEFINE_SPINLOCK(pinmux_spinlock);

void kerSysInitPinmuxInterface(unsigned int interface) {
    unsigned long flags;
    spin_lock_irqsave(&pinmux_spinlock, flags); 
#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148) || defined(CONFIG_BCM963381) || defined(CONFIG_BCM96848) || defined(CONFIG_BCM94908) || defined(CONFIG_BCM963158)
    bcm_init_pinmux_interface(interface);
#endif
    spin_unlock_irqrestore(&pinmux_spinlock, flags); 
}



/***************************************************************************
 * Function Name: kerSysGetUbusFreq
 * Description  : Chip specific computation.
 * Returns      : the UBUS frequency value in MHz.
 ***************************************************************************/
unsigned int kerSysGetUbusFreq(unsigned int miscStrapBus)
{
   unsigned int ubus = UBUS_BASE_FREQUENCY_IN_MHZ;


   return (ubus);

}  /* kerSysGetUbusFreq */


/***************************************************************************
 * Function Name: kerSysGetChipId
 * Description  : Map id read from device hardware to id of chip family
 *                consistent with  BRCM_CHIP
 * Returns      : chip id of chip family
 ***************************************************************************/
int kerSysGetChipId() { 
        int r;
#if   defined(CONFIG_BCM96838)
        r = 0x6838;
#elif defined(CONFIG_BCM96848)
        r = 0x6848;
#elif defined(CONFIG_BCM96858)
        r = 0x6858;
#elif defined(CONFIG_BCM96836)
        r = 0x68360;
#elif defined(CONFIG_BCM960333)
        r = 0x60333;
#elif defined(CONFIG_BCM947189)
        r = 0x47189;
#else
        r = (int) ((PERF->RevID & CHIP_ID_MASK) >> CHIP_ID_SHIFT);
        /* Force BCM63168, BCM63169, and BCM63269 to be BCM63268) */
        if( ( (r & 0xffffe) == 0x63168 )
          || ( (r & 0xffffe) == 0x63268 ))
            r = 0x63268;

        /* Force 6319 to be BCM6318 */
        if (r == 0x6319)
            r = 0x6318;

#endif

        return(r);
}

#if !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189)
/***************************************************************************
 * Function Name: kerSysGetDslPhyEnable
 * Description  : returns true if device should permit Phy to load
 * Returns      : true/false
 ***************************************************************************/
int kerSysGetDslPhyEnable() {
        int id;
        int r = 1;
        id = (int) ((PERF->RevID & CHIP_ID_MASK) >> CHIP_ID_SHIFT);
        if ((id == 0x63169) || (id == 0x63269)) {
            r = 0;
        }
        return(r);
}
#endif

/***************************************************************************
 * Function Name: kerSysGetChipName
 * Description  : fills buf with the human-readable name of the device
 * Returns      : pointer to buf
 ***************************************************************************/
char *kerSysGetChipName(char *buf, int n) {
    return(UtilGetChipName(buf, n));
}

int kerSysGetPciePortEnable(int port)
{
    int ret = 1, dual_lane;
#if defined (CONFIG_BCM96838)
    unsigned int chipId = (PERF->RevID & CHIP_ID_MASK) >> CHIP_ID_SHIFT;

    switch (chipId)
    {
        case 1:        // 68380
        case 6:        // 68380M
        case 7:        // 68389
            ret = 1;
            break;
            
        case 3:        // 68380F
            if(port == 0)
                ret = 1;
            else
                ret = 0;
            break;
        
        case 4:        // 68385
        case 5:        // 68381
        default:
            ret = 0;
            break;
    }
#elif defined (CONFIG_BCM96848)
    unsigned int chipId = (PERF->RevID & CHIP_ID_MASK) >> CHIP_ID_SHIFT;

    switch (chipId)
    {
        case 0x050d:    // 68480F
        case 0x051a:    // 68481P
        case 0x05c0:    // 68486
        case 0x05bd:    // 68485W
        case 0x05be:    // 68488
            ret = 1;
            break;
        default:
            ret = 0;    // 68485, 68481
            break;
    }

    if (port != 0)
        ret = 0;
#elif defined (CONFIG_BCM96858)
    unsigned int chipId;
    bcm_otp_get_chipid(&chipId);

    switch (chipId)
    {
        case 0:
        case 1:     // 68580X
            if ((port == 0) || (port == 1) || ((port == 2) && (MISC->miscStrapBus & MISC_STRAP_BUS_PCIE_SATA_MASK)))
                ret = 1;
            else            
                ret = 0;
            break;
        case 3:     // 68580H
            if ((port == 0) || (port == 1))
                ret = 1;
            else
                ret = 0;
            break;
        case 2:     // 55040
            if (port == 0)
                ret = 1;
            else
                ret = 0;
            break;
        case 4:     // 55040P
                ret = 0;
            break;
        default:
            ret = 0;
            break;
    }
#elif defined (CONFIG_BCM96836)
    if ((port == 0)
        || ((port == 1) && (MISC->miscStrapBus & MISC_STRAP_BUS_PCIE_SINGLE_LANES))
        || ((port == 2) && (MISC->miscStrapBus & MISC_STRAP_BUS_PCIE_SATA_MASK))
       )
        ret = 1;
    else
        ret = 0;
#endif

    /* In case of dual lane on PCIe0, PCIe1 isn't used */
    if (port == 1 && (BpGetPciPortDualLane(0, &dual_lane) == BP_SUCCESS) && dual_lane)
        ret = 0;

    return ret;
}
EXPORT_SYMBOL(kerSysGetPciePortEnable);

int kerSysGetSataPortEnable(void)
{
    
#if defined (CONFIG_BCM96836) || defined(CONFIG_BCM96858)
	/* check if port is configured for SATA */
    if(MISC->miscStrapBus & MISC_STRAP_BUS_PCIE_SATA_MASK)
	{
	    /*confgured for PCIe */
		return 0;
	}
#endif    

#if defined(CONFIG_BCM96836) || defined(CONFIG_BCM96858) || defined(CONFIG_BCM94908)
    {
        unsigned int otp_value = 0;
        if( bcm_otp_is_sata_disabled(&otp_value) == 0 && otp_value )
        { 
            return 0;
        }
    }
#endif       

	return 1;
}
EXPORT_SYMBOL(kerSysGetSataPortEnable);

int kerSysGetUsbHostPortEnable(int port)
{
    int ret = 1;
#if defined (CONFIG_BCM96838)
    unsigned int chipId = (PERF->RevID & CHIP_ID_MASK) >> CHIP_ID_SHIFT;
    
    switch (chipId)
    {
        case 1:        // 68380
        case 6:        // 68380M
        case 7:        // 68389
            ret = 1;
            break;
            
        case 3:        // 68380F
            if(port == 0)
                ret = 1;
            else
                ret = 0;
            break;
        
        case 4:        // 68385
        case 5:        // 68381
        default:
            ret = 0;
            break;
    }
#elif defined (CONFIG_BCM96848)
    unsigned int chipId = (PERF->RevID & CHIP_ID_MASK) >> CHIP_ID_SHIFT;

    switch (chipId)
    {
        case 0x050d:    // 68480F
        case 0x05c0:    // 68486
        case 0x05be:    // 68488
            ret = 1;
            break;
        default:
            ret = 0;
            break;
    }

    if(port != 0)
        ret = 0;
#elif defined (CONFIG_BCM96858)
    unsigned int chipId;
    unsigned int port_disable;

    bcm_otp_get_usb_port_disabled(port, &port_disable);

    if(port_disable)
        return 0;

    bcm_otp_get_chipid(&chipId);

    switch (chipId)
    {
        case 0:
        case 1:    // 68580X
        case 3:    // 68580H
            ret = 1;
            break;
        case 2:    // 55040
        case 4:    // 55040P
            ret = 0;
            break;
        default:
            ret = 0;
            break;
    }
#elif defined (CONFIG_BCM96836)
    ret = 1;
#endif	
    return ret;
}
EXPORT_SYMBOL(kerSysGetUsbHostPortEnable);

int kerSysGetUsbDeviceEnable(void)
{
    int ret = 1;
    
#if defined (CONFIG_BCM96838) || defined (CONFIG_BCM96848) || defined(CONFIG_BCM96858)
    ret = 0;
#endif    

    return ret;
}
EXPORT_SYMBOL(kerSysGetUsbDeviceEnable);

int kerSysGetUsb30HostEnable(void)
{
    int ret = 0;
    
#if defined(CONFIG_BCM963138)|| defined(CONFIG_BCM963148)
    ret = 1;
#endif    

    return ret;
}
EXPORT_SYMBOL(kerSysGetUsb30HostEnable);

int kerSysSetUsbPower(int on, USB_FUNCTION func)
{
    int status = 0;
#if !defined(CONFIG_BRCM_IKOS)
#if defined (CONFIG_BCM96838)
    static int usbHostPwr = 1;
    static int usbDevPwr = 1;
    
    if(on)
    {
        if(!usbHostPwr && !usbDevPwr)
            status = PowerOnZone(PMB_ADDR_USB30_2X, USB30_2X_Zone_Common);
        
        if(((func == USB_HOST_FUNC) || (func == USB_ALL_FUNC)) && !usbHostPwr && !status)
        {
            status = PowerOnZone(PMB_ADDR_USB30_2X, USB30_2X_Zone_USB_Host);
            if(!status)
                usbHostPwr = 1;
        }
        
        if(((func == USB_DEVICE_FUNC) || (func == USB_ALL_FUNC)) && !usbDevPwr && !status)
        {
            status = PowerOnZone(PMB_ADDR_USB30_2X, USB30_2X_Zone_USB_Device);
            if(!status)
                usbDevPwr = 1;
        }
    }
    else
    {
        if(((func == USB_HOST_FUNC) || (func == USB_ALL_FUNC)) && usbHostPwr)
        {
            status = PowerOffZone(PMB_ADDR_USB30_2X, USB30_2X_Zone_USB_Host);
            if(!status)
                usbHostPwr = 0;
        }
        
        if(((func == USB_DEVICE_FUNC) || (func == USB_ALL_FUNC)) && usbDevPwr)
        {
            status = PowerOffZone(PMB_ADDR_USB30_2X, USB30_2X_Zone_USB_Device);
            if(!status)
                    usbDevPwr = 0;
        }
        
        if(!usbHostPwr && !usbDevPwr)
            status = PowerOffZone(PMB_ADDR_USB30_2X, USB30_2X_Zone_Common);
    }
#endif
#endif

    return status;
}
EXPORT_SYMBOL(kerSysSetUsbPower);

extern const struct obs_kernel_param __setup_start[], __setup_end[];
extern const struct kernel_param __start___param[], __stop___param[];

/*
 * Another command line variable...
 * This one to allow /proc/cmdline to show also those boot parameters that
 * are set by drivers using kerSysSetBootParm().
 */
#define BOARD_COMMAND_LINE_LEN 64
char board_command_line[BOARD_COMMAND_LINE_LEN];
EXPORT_SYMBOL(board_command_line);

static void add_to_board_cmd_line(char *name, char *value)
{
    int pos = strlen(board_command_line);

    if (pos < (BOARD_COMMAND_LINE_LEN - 2)) {
        board_command_line[pos++] = ' ';
        pos += strlcpy(&board_command_line[pos], name,
                       BOARD_COMMAND_LINE_LEN - pos);
        if (board_command_line[pos - 1] != '=')
            board_command_line[pos++] = ' ';
        strlcpy(&board_command_line[pos], value,
                BOARD_COMMAND_LINE_LEN - pos);
        board_command_line[BOARD_COMMAND_LINE_LEN - 1] = '\0';
    }
}

void kerSysSetBootParm(char *name, char *value)
{
    const struct obs_kernel_param *okp = __setup_start;
    const struct kernel_param *kp = __start___param;

    do {
        if (!strcmp(name, okp->str)) {
            if (okp->setup_func) {
                okp->setup_func(value);
                add_to_board_cmd_line(name, value);
                return;
            }
        }
        okp++;
    } while (okp < __setup_end);

    do {
        if (!strcmp(name, kp->name)) {
            if (kp->ops->set) {
                kp->ops->set(value, kp);
                add_to_board_cmd_line(name, value);
                return;
            }
        }
        kp++;
    } while (kp < __stop___param);

    if (!strcmp(name, "phantomBootParm=")) {
        add_to_board_cmd_line(value, "");
    }
}
EXPORT_SYMBOL(kerSysSetBootParm);

int board_ioctl_mem_access(BOARD_MEMACCESS_IOCTL_PARMS* parms, char* kbuf, int len)
{
    int i, j;
    unsigned char *cp,*bcp;
    unsigned short *sp,*bsp;
    unsigned int *ip,*bip;
    void *va;

    bcp = (unsigned char *)kbuf;
    bsp = (unsigned short *)bcp;
    bip = (unsigned int *)bcp;

    switch(parms->space) {
        case BOARD_MEMACCESS_IOCTL_SPACE_REG:
            va = ioremap_nocache((long)parms->address, len);
            break;
        case BOARD_MEMACCESS_IOCTL_SPACE_KERN:
            va = (void*)(uintptr_t)parms->address;
            break;
        default:
            va = NULL;
            return EFAULT;
    }
    // printk("memacecssioctl address started %08x mapped to %08x size is %d count is %d\n",(int)parms.address, (int)va,parms.size, parms.count);
    cp = (unsigned char *)va;
    sp = (unsigned short *)((long)va & ~1);
    ip = (unsigned int *)((long)va & ~3);
    for (i=0; i < parms->count; i++) {
        if ((parms->op == BOARD_MEMACCESS_IOCTL_OP_WRITE) 
            || (parms->op == BOARD_MEMACCESS_IOCTL_OP_FILL)) {
            j = 0;
            if (parms->op == BOARD_MEMACCESS_IOCTL_OP_WRITE) 
            {
                j = i;
            }
            switch(parms->size) {
                case 1:
                    cp[i] = bcp[j];
                    break;
                case 2:
                    sp[i] = bsp[j];
                    break;
                case 4:
                    ip[i] = bip[j];
                    break;
            }
        } else {
                switch(parms->size) {
                case 1:
                    bcp[i] = cp[i];
                    break;
                case 2:
                    bsp[i] = sp[i];
                    break;
                case 4:
                    bip[i] = ip[i];
                    break;
            }
        }
    }
    
    if (va != (void*)(uintptr_t)parms->address)
    {
        iounmap(va);
    }
    return 0;
}

void board_util_init(void)
{
    bcmLogSpiCallbacks_t loggingCallbacks;

    g_ulSdramSize = getMemorySize();
    set_mac_info();
    set_gpon_info();

    /* Print status of last reset */
    print_rst_status();

#if defined(CONFIG_BCM96858)
    /* temporary - need to use dynamic clock */
    UBUS4CLK->ClockCtrl = 0x7221;
    UBUS4XRDPCLK->ClockCtrl = 0x7224;
#endif

     kerSysInitMonitorSocket();
     kerSysInitDyingGaspHandler();
     kerSysInitBatteryManagementUnit();
	 inteno_netlink_init();
#if defined(CONFIG_BCM963381) && !IS_ENABLED(CONFIG_BCM_ADSL)
     /* Enable  dsl mips to workaround WD reset issue when dsl is not built */
     /* DSL power up is done in kerSysInitDyingGaspHandler */
     pmc_dsl_mips_enable(1);
#endif

#if defined(CONFIG_BCM_6802_MoCA)
    loggingCallbacks.kerSysSlaveRead   = kerSysBcmSpiSlaveRead;
    loggingCallbacks.kerSysSlaveWrite  = kerSysBcmSpiSlaveWrite;
    loggingCallbacks.bpGet6829PortInfo = NULL;
#endif
    loggingCallbacks.reserveSlave      = BcmSpiReserveSlave;
    loggingCallbacks.syncTrans         = BcmSpiSyncTrans;
    bcmLog_registerSpiCallbacks(loggingCallbacks);

    return;
}

void __exit board_util_deinit(void)
{
    kerSysDeinitDyingGaspHandler();
    kerSysCleanupMonitorSocket();
	inteno_netlink_exit();
    return;
}
