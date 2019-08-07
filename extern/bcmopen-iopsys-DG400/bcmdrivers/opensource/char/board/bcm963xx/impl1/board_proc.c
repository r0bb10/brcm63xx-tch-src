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
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/ctype.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/mtd/mtd.h>

#include <bcmtypes.h>
#include <bcm_map_part.h>
#include <board.h>
#include <boardparms.h>

#include "board_proc.h"
#include "board_image.h"
#include "board_wl.h"
#include "board_wd.h"

#if defined(CONFIG_BCM96838) || defined(CONFIG_BCM96848) || defined(CONFIG_BCM96858)
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
extern int proc_show_rdp_mem(char *buf, char **start, off_t off, int cnt, int *eof, void *data);
#else
int proc_show_rdp_mem( struct file *file, char __user *buf, size_t len, loff_t *pos);
#endif
#endif

#if defined (WIRELESS)
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
static int proc_get_wl_nandmanufacture(char *page, char **start, off_t off, int cnt, int *eof, void *data);
#ifdef BUILD_NAND
static int proc_get_wl_mtdname(char *page, char **start, off_t off, int cnt, int *eof, void *data);
#endif
#else
static ssize_t proc_get_wl_nandmanufacture(struct file * file, char * buff, size_t len, loff_t *offset);
#ifdef BUILD_NAND
static ssize_t proc_get_wl_mtdname(struct file * file, char * buff, size_t len, loff_t *offset);
#endif
#endif
#endif

static void str_to_num(char* in, char *out, int len);
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
static ssize_t proc_get_param(char *page, char **start, off_t off, int cnt, int *eof, void *data);
static ssize_t proc_get_param_string(char *page, char **start, off_t off, int cnt, int *eof, void *data);
static ssize_t proc_set_param(struct file *f, const char *buf, unsigned long cnt, void *data);
static ssize_t proc_set_led(struct file *f, const char *buf, unsigned long cnt, void *data);
#else
static ssize_t proc_get_param(struct file *, char *, size_t, loff_t *);
static ssize_t proc_get_param_string(struct file *, char *, size_t, loff_t *);
static ssize_t proc_set_param(struct file *, const char *, size_t, loff_t *);
static ssize_t proc_set_led(struct file *, const char *, size_t, loff_t *);
static ssize_t proc_set_param_string(struct file *file, const char *buff, size_t len, loff_t *offset);
#endif

#if defined(CONFIG_BCM_WATCHDOG_TIMER)
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
static ssize_t proc_get_watchdog(char *page, char **start, off_t off, int cnt, int *eof, void *data);
static ssize_t proc_set_watchdog(struct file *f, const char *buf, unsigned long cnt, void *data);
#else
static ssize_t proc_get_watchdog(struct file *, char *, size_t, loff_t *);
static ssize_t proc_set_watchdog(struct file *, const char *, size_t, loff_t *);
#endif
#endif /* defined(CONFIG_BCM_WATCHDOG_TIMER) */

int add_proc_files(void);
int del_proc_files(void);

static ssize_t __proc_get_boardid(char *buf, int cnt)
{
    char boardid[NVRAM_BOARD_ID_STRING_LEN];
    kerSysNvRamGetBoardId(boardid);
    sprintf(buf, "%s", boardid);
    return strlen(boardid);
}
static ssize_t __proc_get_socinfo(char *buf, int cnt)
{
    char socname[15] = {0};
    int i;
    int n=0;

    kerSysGetChipName( socname, strlen(socname));

    for( i=0; i< strlen(socname); i++ )
    {
        if(socname[i] == '_')
        {
            socname[i]='\0';
            break;
        }
    }
            
    n += sprintf(buf,   "SoC Name        :BCM%s\n", socname);
    n += sprintf(buf+n, "Revision        :%s\n", &socname[i+1]);

    return n;
}
static ssize_t __proc_get_wan_type(char *buf)
{
    int n = 0;

    unsigned int wan_type = 0, t;
    int i, j, len = 0;

    BpGetOpticalWan(&wan_type);
    if (wan_type == BP_NOT_DEFINED)
    {
        n=sprintf(buf, "none");
        return n;
    }

    for (i = 0, j = 0; wan_type; i++)
    {
        t = wan_type & (1 << i);
        if (!t)
            continue;

        wan_type &= ~(1 << i);
        if (j++)
        {
            sprintf(buf + len, "\n");
            len++;
        }

        switch (t)
        {
        case BP_OPTICAL_WAN_GPON:
            n+=sprintf(buf + len, "gpon");
            break;
        case BP_OPTICAL_WAN_EPON:
            n+=sprintf(buf + len, "epon");
            break;
        case BP_OPTICAL_WAN_AE:
            n+=sprintf(buf + len, "ae");
            break;
        default:
            n+=sprintf(buf + len, "unknown");
            break;
        }
        len += n;
    }

    return len;
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
static ssize_t proc_get_wan_type(char *buf, char **start, off_t off, int cnt, int *eof, void *data)
{
    *eof=1;
    return __proc_get_wan_type(buf);
}
static ssize_t proc_get_boardid(char *buf, char **start, off_t off, int cnt, int *eof, void *data)
{
    *eof=1;
    return __proc_get_boardid(buf, cnt);
}
#else
static ssize_t proc_get_boardid( struct file *file,
                                       char __user *buf,
                                       size_t len, loff_t *pos)
{
    int ret=0;
    if(len && *pos == 0)
    {
       *pos=__proc_get_boardid(buf, len);
           if(likely(*pos != 0)) //something went wrong
        ret=*pos;
    }
    return ret;
}

static ssize_t proc_get_socinfo( struct file *file,
                                       char __user *buf,
                                       size_t len, loff_t *pos)
{
    int ret=0;
    if(len && *pos == 0)
    {
       *pos=__proc_get_socinfo(buf, len);
           if(likely(*pos != 0)) //something went wrong
        ret=*pos;
    }
    return ret;
}
static ssize_t proc_get_wan_type( struct file *file,
                                       char __user *buf,
                                       size_t len, loff_t *pos)
{
    int ret=0;
    if(len && *pos == 0)
    {
       *pos=__proc_get_wan_type(buf);
       if(likely(*pos != 0)) //something went wrong
           ret=*pos;
    }
    return ret;
}
#endif

// Use this ONLY to convert strings of bytes to strings of chars
// use functions from linux/kernel.h for everything else
static void str_to_num(char* in, char* out, int len)
{
    int i;
    memset(out, 0, len);

    for (i = 0; i < len * 2; i ++)
    {
        if ((*in >= '0') && (*in <= '9'))
            *out += (*in - '0');
        else if ((*in >= 'a') && (*in <= 'f'))
            *out += (*in - 'a') + 10;
        else if ((*in >= 'A') && (*in <= 'F'))
            *out += (*in - 'A') + 10;
        else
            *out += 0;

        if ((i % 2) == 0)
            *out *= 16;
        else
            out ++;

        in ++;
    }
    return;
}

static ssize_t __proc_set_param(struct file *f, const char *buf, unsigned long cnt, void *data)
{
    NVRAM_DATA *pNvramData;
    char input[32];

    int i = 0;
    int r = cnt;
    int offset  = ((int *)data)[0];
    int length  = ((int *)data)[1];

    if ((offset < 0) || (offset + length > sizeof(NVRAM_DATA)))
        return 0;

    if ((cnt > 32) || (copy_from_user(input, buf, cnt) != 0))
        return -EFAULT;

    for (i = 0; i < r; i ++)
    {
        if (!isxdigit(input[i]))
        {
            memmove(&input[i], &input[i + 1], r - i - 1);
            r --;
            i --;
        }
    }

    if (NULL != (pNvramData = readNvramData()))
    {
        str_to_num(input, ((char *)pNvramData) + offset, length);
        writeNvramData(pNvramData);
    }
    else
    {
        cnt = 0;
    }


    if (pNvramData)
        kfree(pNvramData);

    return cnt;
}
static ssize_t __proc_get_param(char *page, int cnt, void *data)
{
    int i = 0;
    int r = 0;
    int offset  = ((int *)data)[0];
    int length  = ((int *)data)[1];
    NVRAM_DATA *pNvramData;

    if (!page || !offset || cnt < 1)
        return 0;

    if ((offset < 0) || (offset + length > sizeof(NVRAM_DATA)))
        return 0;

    if (NULL != (pNvramData = readNvramData()))
    {
        for (i = 0; i < length && r + 1 < cnt; i++)
            r += sprintf(page + r, "%02x ", ((unsigned char *)pNvramData)[offset + i]);
    }

    r += sprintf(page + r, "\n");
    if (pNvramData)
        kfree(pNvramData);
    return (r < cnt)? r: 0;


}

/* The string stored in nvram only has a terminating 0 if it is shorter than the
   max allocated space for the variable.
   There could be garbage after the terminating 0 up to max allowed.
   Non initialized data is 0xff.
*/
static ssize_t __proc_get_param_string(char *dst, int dst_len, void *data)
{
        unsigned char *src;
        int offset  = ((int *)data)[0];
        int max_len = ((int *)data)[1];
        int src_len = 0;
        int i;

        NVRAM_DATA *pNvramData;

        /* sanity check for internal data structs */
        if ((offset < 0) || (offset + max_len > sizeof(NVRAM_DATA)))
                goto out_1;

        /* get nvram data */
        if (! (pNvramData = readNvramData()))
                goto out_1;
        src = (char *)(pNvramData) + offset;

        /* find out how many chars of real data in nvram string (not a C string, might not have a terminating 0 ) */
        for (i=0 ; i<max_len ; i++){
                if (src[i] == 0 || src[i] == 0xff )
                        break;
                src_len++;
        }

        /* is the read buffer large enough (including newline char). if not do not return partial answer */
        if (dst_len < src_len + 1)
                goto out_2;

        /* raw string no \0 no \n */
        if ( copy_to_user(dst, src, src_len)){
                src_len = -EFAULT;
                goto out_2;
        }

        /* add newline */
        if ( copy_to_user(dst + src_len, "\n", 1)){
                src_len = -EFAULT;
                goto out_2;
        }
        src_len++;

out_2:
        kfree(pNvramData);
out_1:
        return src_len;
}

static ssize_t __proc_get_param_int(char *dst, int dst_len, void *args)
{
    const int FORMATTING_LEN = 4;                                               // Overhead of ascii 0x....\n\0
    int srcOffset  = ((int*)args)[0];
    int srcLen  = ((int*)args)[1];
    unsigned int *src, retLen;
    NVRAM_DATA *pNvramData;

    if (!dst || !dst_len)
        return 0;

    if (srcOffset < 0 || srcOffset + srcLen > sizeof(NVRAM_DATA))
        return 0;

    pNvramData = readNvramData();
    if(!pNvramData)
        return 0;

    src = (unsigned int*) ((char*)(pNvramData) + srcOffset);

    retLen = FORMATTING_LEN + srcLen * 2;                                       // Times two due to each byte gives two ascii chars.
    if(dst_len < retLen)
        retLen = dst_len;

    snprintf(dst, retLen, "0x%.8x\n", *src);

    kfree(pNvramData);
    return retLen;
}


static ssize_t __proc_set_param_int(const char *src, size_t srcLen, void *args)
{
    int dstOffset  = ((int*)args)[0];
    int dstLen  = ((int*)args)[1];
    NVRAM_DATA *pNvramData;
    unsigned int val, *dst;

    if (!src || !srcLen)
        return 0;

    if (dstOffset < 0 || dstOffset + dstLen > sizeof(NVRAM_DATA) ||
            dstLen != sizeof(unsigned int))
        return 0;

    pNvramData = readNvramData();
    if(!pNvramData)
        return 0;

    if (sscanf(src, "%10x", &val) == 1) {
        dst = (unsigned int*) ((char*)(pNvramData) + dstOffset);
        *dst = val;
        writeNvramData(pNvramData);
    }
    else {
        srcLen = -EINVAL;
    }

    kfree(pNvramData);
    return srcLen;
}


static ssize_t __proc_set_led(struct file *f, const char *buf, unsigned long cnt, void *data)
{
    // char leddata[16];
    unsigned int leddata;
    char input[32];
    int i;

    if (cnt > 31)
        cnt = 31;

    if (copy_from_user(input, buf, cnt) != 0)
        return -EFAULT;


    for (i = 0; i < cnt; i ++)
    {
        if (!isxdigit(input[i]))
        {
            input[i] = 0;
        }
    }
    input[i] = 0;

    if (0 != kstrtouint(input, 16, &leddata)) 
        return -EFAULT;

    kerSysLedCtrl ((leddata & 0xff00) >> 8, leddata & 0xff);
    return cnt;
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
static ssize_t proc_get_param(char *page, char **start, off_t off, int cnt, int *eof, void *data)
{
    *eof = 1;
    return __proc_get_param(page, cnt, data); 

}

static ssize_t proc_get_param_string(char *page, char **start, off_t off, int cnt, int *eof, void *data)
{
    *eof = 1;
    return __proc_get_param_string(page, cnt, data); 

}


static ssize_t proc_set_param(struct file *f, const char *buf, unsigned long cnt, void *data)
{
    return __proc_set_param(f,buf,cnt,data);
}

/*
 * This function expect input in the form of:
 * echo "xxyy" > /proc/led
 * where xx is hex for the led number
 * and   yy is hex for the led state.
 * For example,
 *     echo "0301" > led
 * will turn on led 3
 */
static ssize_t proc_set_led(struct file *f, const char *buf, unsigned long cnt, void *data)
{
    return __proc_set_led(f, buf, cnt, data);
}
#else
static ssize_t proc_get_param(struct file * file, char * buff, size_t len, loff_t *offset)
{

    int ret=0;
    if(len && *offset == 0)
    {
        *offset =__proc_get_param(buff, len, PDE_DATA(file_inode(file))); 
        if(likely(*offset != 0)) //something went wrong
            ret=*offset;
    }
    return ret;
}
static ssize_t proc_get_param_string(struct file * file, char * buff, size_t len, loff_t *offset)
{

    int ret=0;
    if(*offset == 0)
    {
        *offset =__proc_get_param_string(buff, len, PDE_DATA(file_inode(file))); 
        if(likely(*offset != 0)) //something went wrong
            ret=*offset;
    }
    return ret;
}

static ssize_t proc_get_param_int(struct file *file, char *buf, size_t len, loff_t *offset)
{

    int ret = 0;
    if(*offset == 0)
    {
        *offset = __proc_get_param_int(buf, len, PDE_DATA(file_inode(file))); 
        if(likely(*offset != 0)) //something went wrong
            ret = *offset;
    }
    return ret;
}

static ssize_t proc_set_param(struct file *file, const char *buff, size_t len, loff_t *offset)
{
    return __proc_set_param(file,buff,len,PDE_DATA(file_inode(file)));
}

static int __proc_set_param_string(struct file *f, const char *buf, unsigned long cnt, void *data)
{
    NVRAM_DATA *pNvramData;
    char input[256];

    int offset  = ((int *)data)[0];
    int length  = ((int *)data)[1];

    if ((offset < 0) || (offset + length > sizeof(NVRAM_DATA)))
        return 0;

    if ((cnt > 256) || (copy_from_user(input, buf, cnt) != 0))
        return -EFAULT;

    if (cnt-1 > length) {
        printk(KERN_ERR "Input string length %lu > field size %d\n", cnt-1, length);
        return -EFAULT;
    }

    if (NULL != (pNvramData = readNvramData()))
    {
        /*  set the buffer to '\0' */
        memset(((char *)pNvramData) + offset, 0, length);
        /* copy_from_user gives a '\n' at the end of the buffer, don't copy that */
        memcpy(((char *)pNvramData) + offset, input, cnt-1);
        writeNvramData(pNvramData);
    }
    else
    {
        cnt = 0;
    }

    if (pNvramData)
        kfree(pNvramData);

    return cnt;
}
static ssize_t proc_set_param_string(struct file *file, const char *buff, size_t len, loff_t *offset)
{
    return __proc_set_param_string(file, buff, len, PDE_DATA(file_inode(file)));
}

static ssize_t proc_set_param_int(struct file *file, const char *buf, size_t len, loff_t *offset)
{
    return __proc_set_param_int(buf, len, PDE_DATA(file_inode(file)));
}

static ssize_t proc_set_led(struct file *file, const char *buff, size_t len, loff_t *offset)
{
    int ret=-1;
    if(*offset == 0)
    {
        *offset=__proc_set_led(file, buff, len, PDE_DATA(file_inode(file)));
        if(likely(*offset != 0)) //something went wrong
            ret=*offset;
    }    
    return ret;

}
#endif

#if defined(CONFIG_BCM_WATCHDOG_TIMER)

#if defined(CONFIG_BCM963268)
#  define GET_RESET_STATUS()  (TIMER->ClkRstCtl)
#  define RESET_STATUS_POR   (1<<29)
#  define RESET_STATUS_HW    (1<<30) /* correct bits despite Broadcom has other constants in 63268_map_part.h */
#  define RESET_STATUS_SW    (1<<31)
#elif defined(CONFIG_BCM963138)
#  define GET_RESET_STATUS()  (TIMER->ResetStatus)
#  define RESET_STATUS_POR   POR_RESET_STATUS
#  define RESET_STATUS_HW    HW_RESET_STATUS
#  define RESET_STATUS_SW    SW_RESET_STATUS
#  define STRAP_RESET_MASK   0x3000
#  define STRAP_RESET_POR    0x1000
#  define STRAP_RESET_HW     0x2000
#  define STRAP_RESET_SW     0x3000
#elif defined(CONFIG_BCM96362) || defined(CONFIG_BCM963381)
#  define GET_RESET_STATUS()  (0)  /* This chip has no reset status reg */
#  define RESET_STATUS_POR   (1)
#  define RESET_STATUS_HW    (1)
#  define RESET_STATUS_SW    (1)
#else
#  error "You need to figure out what the register name/bits are for this chip!"
#endif

static ssize_t __proc_get_watchdog(char *page, int cnt, void *data)
{
    int r = 0;

	if(!page || !cnt) return 0;
    r += sprintf(page + r, "watchdog enabled=%u timer=%u us suspend=%u\n", 
                           watchdog_data.enabled, 
                           watchdog_data.timer, 
                           watchdog_data.suspend);
    r += sprintf(page + r, "         userMode=%u userThreshold=%u userTimeout=%u\n", 
                           watchdog_data.userMode, 
                           watchdog_data.userThreshold/2, 
                           watchdog_data.userTimeout/2);

    /* Special case for 63138 where Broadcom does a reboot
     * in init_arm.S every time the system starts. Likely
     * for increasing system stability. */
#if defined(CONFIG_BCM963138)
    if(MISC->miscStrapBus & MISC_STRAP_BUS_PMC_BOOT_AVS &&
            GET_RESET_STATUS() & RESET_STATUS_SW) {
        switch (MISC->miscStrapBus & STRAP_RESET_MASK) {
            case STRAP_RESET_POR:
                r += sprintf(page + r, "boot reason=POR\n");
                break;
            case STRAP_RESET_HW:
                r += sprintf(page + r, "boot reason=HW\n");
                break;
            case STRAP_RESET_SW:
                r += sprintf(page + r, "boot reason=SW\n");
                break;
            default:
                r += sprintf(page + r, "boot reason=??\n");
                break;
        }

        return (r < cnt)? r: 0;
    }
#endif

    if (GET_RESET_STATUS() & RESET_STATUS_POR)
            r += sprintf(page + r, "boot reason=POR\n");
    else if (GET_RESET_STATUS() & RESET_STATUS_HW)
            r += sprintf(page + r, "boot reason=HW\n");
    else if (GET_RESET_STATUS() & RESET_STATUS_SW)
            r += sprintf(page + r, "boot reason=SW\n");
    else
            r += sprintf(page + r, "boot reason=??\n");

    return (r < cnt)? r: 0;
}

static ssize_t __proc_set_watchdog(struct file *f, const char *buf, unsigned long cnt, void *data)
{
    char input[64];
    unsigned int enabled, timer;
    unsigned int userMode, userThreshold;
   
    if (cnt > 64) 
    {
        cnt = 64;
    }

    if (copy_from_user(input, buf, cnt) != 0) 
    {
        return -EFAULT;
    }

    if (strncasecmp(input, "OK", 2) == 0)
    {
        bcm_reset_watchdog();
        return cnt;
    }

    if (sscanf(input, "%u %u %u %u", &enabled, &timer, &userMode, &userThreshold) != 4)
    {
        printk("\nError format, it is as:\n");
        printk("\"enabled(0|1) timer(us) userMode(0|1) userThreshold\"\n");
        return -EFAULT;
    }

    bcm_set_watchdog(enabled, timer, userMode, userThreshold);

    return cnt;
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
static ssize_t proc_get_watchdog(char *page, char **start, off_t off, int cnt, int *eof, void *data)
{
    *eof = 1;
    return __proc_get_watchdog(page, cnt, data);
}

static ssize_t proc_set_watchdog(struct file *f, const char *buf, unsigned long cnt, void *data)
{
    return __proc_set_watchdog(f, buf, cnt, data);
}
#else
static ssize_t proc_get_watchdog(struct file *file, char *buff, size_t len, loff_t *offset)
{

    if(*offset != 0)
        return 0;
    *offset = __proc_get_watchdog(buff, len, PDE_DATA(file_inode(file)));

    return *offset;

}
static ssize_t proc_set_watchdog (struct file *file, const char *buff, size_t len, loff_t *offset)
{
    int ret=-1;

    if(*offset == 0)
    {
       *offset=__proc_set_watchdog(file, buff, len, PDE_DATA(file_inode(file)));
       if(likely(*offset != 0)) //something went wrong
          ret=*offset;
    }
return ret;
}
#endif
#endif


#if defined(WIRELESS)
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
static int proc_get_wl_nandmanufacture(char *page, char **start, off_t off, int cnt, int *eof, void *data)
{
    int r = 0;
    r += sprintf(page + r, "%d", _get_wl_nandmanufacture());
    return (r < cnt)? r: 0;
}

#ifdef BUILD_NAND
static int proc_get_wl_mtdname(char *page, char **start, off_t off, int cnt, int *eof, void *data)
{
    int r=0;
    struct mtd_info *mtd = get_mtd_device_nm(WLAN_MFG_PARTITION_NAME);
    if( !IS_ERR_OR_NULL(mtd) )
            r += sprintf(page + r, "mtd%d",mtd->index );
    return (r < cnt)? r: 0;
}
#endif

#else
/*  for higher version 4.1 kernel */
static ssize_t proc_get_wl_nandmanufacture(struct file * file, char * buff, size_t len, loff_t *pos)
{
    ssize_t ret=0;
    if(len && *pos == 0)
    {
        (*pos) = sprintf(buff, "%d", _get_wl_nandmanufacture());
        if(likely(*pos != 0)) 
            ret=*pos;
    }
    return ret;
}

#ifdef BUILD_NAND
static ssize_t proc_get_wl_mtdname(struct file * file, char * buff, size_t len, loff_t *pos)
{
    ssize_t ret=0;
    if(len && *pos == 0)
    {
        struct mtd_info *mtd = get_mtd_device_nm(WLAN_MFG_PARTITION_NAME);
        if( !IS_ERR_OR_NULL(mtd) ) {
           (*pos) = sprintf(buff, "mtd%d",mtd->index );
           if(likely(*pos != 0)) 
               ret=*pos;
         }
    }
    return ret;
}
#endif
#endif
#endif

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
#else
   static struct file_operations proc_ro_fops = {
       .read = proc_get_param,
       .write = NULL,
    };
   static struct file_operations proc_rw_fops = {
       .read = proc_get_param,
       .write = proc_set_param,
    };
   static struct file_operations proc_ro_str_fops = {
       .read = proc_get_param_string,
       .write = NULL,
    };
   static struct file_operations proc_rw_str_fops = {
       .read = proc_get_param_string,
       .write = proc_set_param_string,
    };
   static struct file_operations proc_rw_int_fops = {
       .read = proc_get_param_int,
       .write = proc_set_param_int,
    };
#ifdef WIRELESS
   static struct file_operations wl_get_nandmanufacture_proc = {
       .read=proc_get_wl_nandmanufacture,
       .write=NULL,
    };
#ifdef BUILD_NAND
   static struct file_operations wl_get_mtdname_proc = {
       .read=proc_get_wl_mtdname,
       .write=NULL,
    };
#endif
#endif
   static struct file_operations base_mac_add_proc = {
       .read=proc_get_param,
       .write=proc_set_param,
    };
   static struct file_operations bootline_proc = {
       .read=proc_get_param_string,
       .write=NULL,
    };
    static struct file_operations led_proc = {
       .write=proc_set_led,
    };
#if defined(CONFIG_BCM96838) || defined(CONFIG_BCM96848)
    static struct file_operations rdp_mem_proc = {
       .read=proc_show_rdp_mem,
    };
#endif
    static struct file_operations supp_optical_wan_types_proc = {
       .read=proc_get_wan_type,
    };
#if defined(CONFIG_BCM_WATCHDOG_TIMER)
    static struct file_operations watchdog_proc = {
       .read=proc_get_watchdog,
       .write=proc_set_watchdog,
    };
#endif
    static struct file_operations boardid_proc = {
       .read=proc_get_boardid,
    };
    static struct file_operations socinfo_proc = {
       .read=proc_get_socinfo,
    };
#endif

int add_proc_files(void)
{
#define offset(type, elem) ((size_t)&((type *)0)->elem)

    static int BaseMacAddr[2] = {offset(NVRAM_DATA, ucaBaseMacAddr), NVRAM_MAC_ADDRESS_LEN};
    static int Version[2]      = {offset(NVRAM_DATA, ulVersion), sizeof(unsigned long)};
    static int Bootline[2]     = {offset(NVRAM_DATA, szBootline), NVRAM_BOOTLINE_LEN};
    static int BoardId[2]      = {offset(NVRAM_DATA, szBoardId), NVRAM_BOARD_ID_STRING_LEN};
    static int gponSerialNumber[2] = {offset(NVRAM_DATA, gponSerialNumber), NVRAM_GPON_SERIAL_NUMBER_LEN};
    static int gponPassword[2] = {offset(NVRAM_DATA, gponPassword), NVRAM_GPON_PASSWORD_LEN};
    static int wpsDevicePin[2] = {offset(NVRAM_DATA, wpsDevicePin), NVRAM_WPS_DEVICE_PIN_LEN};
    static int wlanParams[2]   = {offset(NVRAM_DATA, wlanParams), NVRAM_WLAN_PARAMS_LEN};
    static int wlanFeature[2]  = {offset(NVRAM_DATA, wlanParams)+NVRAM_WLAN_PARAMS_LEN-1, sizeof(char)};
    static int VoiceBoardId[2] = {offset(NVRAM_DATA, szVoiceBoardId), NVRAM_BOARD_ID_STRING_LEN};
    static int AuthKey[2]       = {offset(NVRAM_DATA, szAuthKey), NVRAM_AUTH_KEY_LEN};
    static int DesKey[2]       = {offset(NVRAM_DATA, szDesKey), NVRAM_DES_KEY_LEN};
    static int WpaKey[2]       = {offset(NVRAM_DATA, szWpaKey), NVRAM_WPA_KEY_LEN};
    static int SerialNumber[2] = {offset(NVRAM_DATA, szSerialNumber), NVRAM_SERIAL_NUMBER_LEN};
    static int rfpi[2]         = {offset(NVRAM_DATA, rfpi), RFPI_LEN};
    static int bcm_mod_dev[2]  = {offset(NVRAM_DATA, bcm_mod_dev), BCM_MOD_DEV_LEN};
    static int bcm_def_freq[2] = {offset(NVRAM_DATA, bcm_def_freq), BCM_DEF_FREQ_LEN};
    static int fixed_emc[2]    = {offset(NVRAM_DATA, fixed_emc), FIXED_EMC_LEN};
    static int dslAnnex[2]     = {offset(NVRAM_DATA, dslAnnex), sizeof(char)};
    static int iVersion[2]     = {offset(NVRAM_DATA, iVersion), sizeof(char)};
    static int iAntenna[2]     = {offset(NVRAM_DATA, iAntenna), sizeof(char)};
    static int psn[2]          = {offset(NVRAM_DATA, psn), NVRAM_PSN_LEN};
    static int pcbasn[2]       = {offset(NVRAM_DATA, pcbasn), NVRAM_PCBASN_LEN};
    static int ulBoardStuffOption[2] = {offset(NVRAM_DATA, ulBoardStuffOption), sizeof(unsigned int)};
    static int NumMacAddrs[2]  = {offset(NVRAM_DATA, ulNumMacAddrs), sizeof(unsigned long)};
    static int hwVer[2]  = {offset(NVRAM_DATA, hwVer), NVRAM_HWVER_LEN};
    static int production[2]   = {offset(NVRAM_DATA, production), sizeof(char)};

    struct proc_dir_entry *p0;
    struct proc_dir_entry *p1;
    struct proc_dir_entry *p2;
#if defined(CONFIG_BCM96838) || defined(CONFIG_BCM96848)
    struct proc_dir_entry *p3;
#endif
    struct proc_dir_entry *p4;
    struct proc_dir_entry *p5;

    p0 = proc_mkdir("nvram", NULL);

    if (p0 == NULL)
    {
        printk("add_proc_files: failed to create proc files!\n");
        return -1;
    }

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11)
#if defined(WIRELESS)
    p1 = create_proc_entry("wl_nand_manufacturer", 0444, p0);
    if (p1 == NULL)
    {
        printk("add_proc_files: failed to create proc files!\n");
        return -1;
    }
    p1->read_proc   = proc_get_wl_nandmanufacture;
    p1->write_proc =NULL;

#ifdef BUILD_NAND

    p1 = create_proc_entry("wl_nand_mtdname", 0444, p0);
    if (p1 == NULL)
    {
        printk("add_proc_files: failed to create proc files!\n");
        return -1;
    }
    p1->read_proc   = proc_get_wl_mtdname;
    p1->write_proc =NULL;
#endif
#endif
    p1 = create_proc_entry("BaseMacAddr", 0644, p0);

    if (p1 == NULL)
    {
        printk("add_proc_files: failed to create proc files!\n");
        return -1;
    }

    p1->data        = BaseMacAddr;
    p1->read_proc   = proc_get_param;
    p1->write_proc  = proc_set_param;

    p1 = create_proc_entry("bootline", 0644, p0);

    if (p1 == NULL)
    {
        printk("add_proc_files: failed to create proc files!\n");
        return -1;
    }

    p1->data        = (void *)offset(NVRAM_DATA, szBootline);
    p1->read_proc   = proc_get_param_string;
    p1->write_proc  = NULL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
    //New linux no longer requires proc_dir_entry->owner field.
#else
    p1->owner       = THIS_MODULE;
#endif

    p1 = create_proc_entry("led", 0644, NULL);
    if (p1 == NULL)
        return -1;

    p1->write_proc  = proc_set_led;

#if defined(CONFIG_BCM96838) || defined(CONFIG_BCM96848)
    p3 = create_proc_entry("show_rdp_mem", 0444, NULL);
    if (p3 == NULL)
        return -1;

    p3->read_proc  = proc_show_rdp_mem;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
    //New linux no longer requires proc_dir_entry->owner field.
#else
    p1->owner       = THIS_MODULE;
#endif

    p2 = create_proc_entry("supported_optical_wan_types", 0444, p0);
    if (p2 == NULL)
        return -1;
    p2->read_proc = proc_get_wan_type;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
    //New linux no longer requires proc_dir_entry->owner field.
#else
    p2->owner       = THIS_MODULE;
#endif

#if defined(CONFIG_BCM_WATCHDOG_TIMER)
    p2 = create_proc_entry("watchdog", 0644, NULL);
    if (p2 == NULL)
    {
        printk("add_proc_files: failed to create watchdog proc file!\n");
        return -1;
    }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
    //New linux no longer requires proc_dir_entry->owner field.
#else
    p2->owner       = THIS_MODULE;
#endif

    p2->data        = NULL;
    p2->read_proc   = proc_get_watchdog;
    p2->write_proc  = proc_set_watchdog;
#endif /* defined(CONFIG_BCM_WATCHDOG_TIMER) */

    p4 = create_proc_entry("boardid", 0444, p0);
    if (p4 == NULL)
        return -1;
    p4->read_proc = proc_get_boardid;
#else /* LINUX_VERSION_CODE <= KERNEL_VERSION(3,4,11) */

    if(!proc_create_data("Version", 0644, p0, &proc_ro_fops, Version))
        goto err_create;

	if(!proc_create_data("ulBoardStuffOption", 0644, p0,
            &proc_rw_int_fops, ulBoardStuffOption))
        goto err_create;

    if(!proc_create_data("Bootline", 0644, p0, &proc_rw_str_fops, Bootline))
        goto err_create;

    if(!proc_create_data("BoardId", 0644, p0, &proc_rw_str_fops, BoardId))
        goto err_create;

    if(!proc_create_data("gponSerialNumber", 0644, p0, &proc_ro_str_fops,
            gponSerialNumber))
        goto err_create;

    if(!proc_create_data("gponPassword", 0644, p0, &proc_ro_str_fops,
            gponPassword))
        goto err_create;

    if(!proc_create_data("wpsDevicePin", 0644, p0, &proc_rw_str_fops,
            wpsDevicePin))
        goto err_create;

    if(!proc_create_data("wlanParams", 0644, p0, &proc_ro_str_fops,
            wlanParams))
        goto err_create;

    if(!proc_create_data("wlanFeature", 0644, p0, &proc_rw_fops,
            wlanFeature))
        goto err_create;

    if(!proc_create_data("VoiceBoardId", 0644, p0, &proc_rw_str_fops,
            VoiceBoardId))
        goto err_create;

    if(!proc_create_data("AuthKey", 0644, p0, &proc_rw_str_fops, AuthKey))
        goto err_create;

    if(!proc_create_data("DesKey", 0644, p0, &proc_rw_str_fops, DesKey))
        goto err_create;

    if(!proc_create_data("WpaKey", 0644, p0, &proc_rw_str_fops, WpaKey))
        goto err_create;

    if(!proc_create_data("SerialNumber", 0644, p0, &proc_rw_str_fops,
            SerialNumber))
        goto err_create;

    if(!proc_create_data("rfpi", 0644, p0, &proc_rw_fops, rfpi))
        goto err_create;

    if(!proc_create_data("bcm_mod_dev", 0644, p0, &proc_rw_fops,
            bcm_mod_dev))
        goto err_create;

    if(!proc_create_data("bcm_def_freq", 0644, p0, &proc_rw_fops,
            bcm_def_freq))
        goto err_create;

    if(!proc_create_data("fixed_emc", 0644, p0, &proc_rw_fops,
            fixed_emc))
        goto err_create;

    if(!proc_create_data("dslAnnex", 0644, p0, &proc_rw_str_fops,
            dslAnnex))
        goto err_create;

    if(!proc_create_data("iVersion", 0644, p0, &proc_rw_fops,
            iVersion))
        goto err_create;

    if(!proc_create_data("iAntenna", 0644, p0, &proc_rw_fops,
            iAntenna))
        goto err_create;

    if(!proc_create_data("psn", 0644, p0, &proc_rw_str_fops,
            psn))
        goto err_create;

    if(!proc_create_data("pcbasn", 0644, p0, &proc_rw_str_fops,
            pcbasn))
        goto err_create;

    if(!proc_create_data("NumMacAddrs", 0644, p0, &proc_rw_fops,
            NumMacAddrs))
        goto err_create;
	
    if(!proc_create_data("HV", 0644, p0, &proc_rw_str_fops,
            hwVer))
        goto err_create;

    if(!proc_create_data("Production", 0644, p0, &proc_rw_fops,
            production))
        goto err_create;

#if defined(WIRELESS)
    p1 = proc_create("wl_nand_manufacturer", S_IRUSR, p0,&wl_get_nandmanufacture_proc);
    if (p1 == NULL)
    {
        printk("add_proc_files: failed to create proc files!\n");
        return -1;
    }

#ifdef BUILD_NAND

    p1 = proc_create("wl_nand_mtdname", S_IRUSR, p0,&wl_get_mtdname_proc);
    if (p1 == NULL)
    {
        printk("add_proc_files: failed to create proc files!\n");
        return -1;
    }
#endif
#endif
     p1 = proc_create_data("BaseMacAddr", S_IRUSR, p0, &base_mac_add_proc, BaseMacAddr);

    if (p1 == NULL)
    {
        printk("add_proc_files: failed to create proc files!\n");
        return -1;
    }

     p1 = proc_create_data("bootline", S_IRUSR, p0, &bootline_proc, Bootline);

    if (p1 == NULL)
    {
        printk("add_proc_files: failed to create proc files!\n");
        return -1;
    }

    p1 = proc_create("led", S_IWUSR, NULL, &led_proc);
    if (p1 == NULL)
        return -1;

#if defined(CONFIG_BCM96838) || defined(CONFIG_BCM96848) 
    p3 = proc_create("show_rdp_mem", S_IRUSR, p0, &rdp_mem_proc);
    if (p3 == NULL)
        return -1;
#endif

    p2 = proc_create("supported_optical_wan_types", S_IRUSR, p0, &supp_optical_wan_types_proc);
    if (p2 == NULL)
        return -1;

#if defined(CONFIG_BCM_WATCHDOG_TIMER)
    p2 = proc_create("watchdog", S_IRUSR|S_IWUSR, p0, &watchdog_proc);
    if (p2 == NULL)
    {
        printk("add_proc_files: failed to create watchdog proc file!\n");
        return -1;
    }
#endif /* defined(CONFIG_BCM_WATCHDOG_TIMER) */

    p4 = proc_create("boardid", S_IRUSR, p0, &boardid_proc);
    if (p4 == NULL)
        return -1;

    p5 = proc_create("socinfo", S_IRUSR, NULL, &socinfo_proc);
    if (p5 == NULL)
        return -1;
#endif

    return 0;
err_create:
    printk("add_proc_files: failed to create proc files!\n");
    return -1;
}

int del_proc_files(void)
{
    remove_proc_entry("nvram", NULL);
    remove_proc_entry("led", NULL);

    return 0;
}
