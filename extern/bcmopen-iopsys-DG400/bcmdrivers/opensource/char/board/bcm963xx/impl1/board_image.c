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
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/ctype.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/syscalls.h>
#include <linux/crc32.h>
#include <linux/mtd/mtd.h>
#include <linux/bcm_assert_locks.h>
#include <linux/jffs2.h>
#include <linux/kmod.h>

#include <bcmtypes.h>
#include <bcm_map_part.h>
#define  BCMTAG_EXE_USE
#include <bcmTag.h>
#include <board.h>
#include <boardparms.h>
#include <boardparms_voice.h>
#include <flash_api.h>
#include <bcm_intr.h>
#include <flash_common.h>

#if defined(CONFIG_MTD_NAND)
#include <linux/mtd/partitions.h>
#include "bcm_crc.c"
#include "bcm_ubi.c"
#define PRINTK(...)
#endif

#include "board_image.h"
#include "board_wl.h"
#include "board_util.h"
#include "board_wd.h"

#include "bcm_otp.h"

#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148)
#include "pmc_drv.h"
#endif

#define IMAGE_VERSION_FILE_NAME "/etc/image_version"
#define IMAGE_VERSION_MAX_SIZE  64
static char imageVersions[2][IMAGE_VERSION_MAX_SIZE] = {{'\0'}, {'\0'}};

extern struct semaphore semflash;
extern struct mutex spMutex;
extern FLASH_ADDR_INFO fInfo;
extern int flash_type;

/*
 * flashImageMutex must be acquired for all write operations to
 * nvram, CFE, or fs+kernel image.  (cfe and nvram may share a sector).
 */
DEFINE_MUTEX(flashImageMutex);

/***************************************************************************
// Function Name: genCrc32
// Description  : caculate the CRC 32 of the given data.
// Parameters   : pdata - array of data.
//                size - number of input data bytes.
//                crc - either CRC32_INIT_VALUE or previous return value.
// Returns      : crc.
****************************************************************************/
static UINT32 genCrc32(byte *pdata, UINT32 size, UINT32 crc)
{
    while (size-- > 0)
        crc = (crc >> 8) ^ Crc32_table[(crc ^ *pdata++) & 0xff];

    return crc;
}

/** calculate the CRC for the nvram data block and write it to flash.
 * Must be called with flashImageMutex held.
 */
int writeNvramDataCrcLocked(PNVRAM_DATA pNvramData)
{
    UINT32 crc = CRC32_INIT_VALUE;

    BCM_ASSERT_HAS_MUTEX_C(&flashImageMutex);

    pNvramData->ulCheckSum = 0;
    crc = genCrc32((char *)pNvramData, sizeof(NVRAM_DATA), crc);
    pNvramData->ulCheckSum = crc;
    return kerSysNvRamSet((char *)pNvramData, sizeof(NVRAM_DATA), 0);
}

int writeNvramData(PNVRAM_DATA pNvramData)
{
    int rc = 0;

    mutex_lock(&flashImageMutex);
    rc = writeNvramDataCrcLocked(pNvramData);
    mutex_unlock(&flashImageMutex);

    return rc;
}


/** read the nvramData struct from the in-memory copy of nvram.
 * The caller is not required to have flashImageMutex when calling this
 * function.  However, if the caller is doing a read-modify-write of
 * the nvram data, then the caller must hold flashImageMutex.  This function
 * does not know what the caller is going to do with this data, so it
 * cannot assert flashImageMutex held or not when this function is called.
 *
 * @return pointer to NVRAM_DATA buffer which the caller must free
 *         or NULL if there was an error
 */
PNVRAM_DATA readNvramData(void)
{
    UINT32 crc = CRC32_INIT_VALUE, savedCrc;
    NVRAM_DATA *pNvramData;

    // use GFP_ATOMIC here because caller might have flashImageMutex held
    if (NULL == (pNvramData = kmalloc(sizeof(NVRAM_DATA), GFP_ATOMIC)))
    {
        printk("readNvramData: could not allocate memory\n");
        return NULL;
    }

    kerSysNvRamGet((char *)pNvramData, sizeof(NVRAM_DATA), 0);
    savedCrc = pNvramData->ulCheckSum;
    pNvramData->ulCheckSum = 0;
    crc = genCrc32((char *)pNvramData, sizeof(NVRAM_DATA), crc);
    if (savedCrc != crc)
    {
        printk("readNvramData: CRC doesnt match 0x%08x 0x%08x\n", savedCrc, crc);
        // this can happen if we write a new cfe image into flash.
        // The new image will have an invalid nvram section which will
        // get updated to the inMemNvramData.  We detect it here and
        // commonImageWrite will restore previous copy of nvram data.
        kfree(pNvramData);
        pNvramData = NULL;
    }

    return pNvramData;
}

// flash bcm image 
// return: 
// 0 - ok
// !0 - the sector number fail to be flashed (should not be 0)
// Must be called with flashImageMutex held.
int kerSysBcmImageSet( int flash_start_addr, char *string, int size,
    int should_yield)
{
    int sts;
    int sect_size;
    int blk_start;
    int savedSize = size;
    int whole_image = 0;
    unsigned long flags=0;
    int is_cfe_write=0;
    WFI_TAG wt = {0};
#if defined(CONFIG_BCM963268)
    PNVRAM_DATA pnd;
#endif

    BCM_ASSERT_HAS_MUTEX_C(&flashImageMutex);
    if (flash_start_addr == FLASH_BASE)
    {
        unsigned int chip_id = kerSysGetChipId();
        whole_image = 1;
        memcpy(&wt, string + size, sizeof(wt));
        if( (wt.wfiVersion & WFI_ANY_VERS_MASK) == WFI_ANY_VERS &&
            wt.wfiChipId != chip_id )
        {
            int id_ok = 0;
#if defined(CHIP_FAMILY_ID_HEX)
            if (wt.wfiChipId == CHIP_FAMILY_ID_HEX) {
                id_ok = 1;
            }
#endif
            if (id_ok == 0) {
                printk("Chip Id error.  Image Chip Id = %04x, Board Chip Id = "
                    "%04x.\n", wt.wfiChipId, chip_id);
                return -1;
            }
        }
    }

    if( whole_image && (wt.wfiVersion & WFI_ANY_VERS_MASK) == WFI_ANY_VERS &&
        wt.wfiFlashType != WFI_NOR_FLASH )
    {
        printk("ERROR: Image does not support a NOR flash device.\n");
        return -1;
    }

#if defined(CONFIG_BCM963268)
    if ( whole_image && ( (   (wt.wfiFlags & WFI_FLAG_SUPPORTS_BTRM)  && (! bcm_otp_is_btrm_boot())) ||
                          ((! (wt.wfiFlags & WFI_FLAG_SUPPORTS_BTRM)) &&    bcm_otp_is_btrm_boot() ) ))
    {
        printk("ERROR: Secure Boot Conflict. The image type does not match the SoC OTP configuration. Aborting.\n");
        return -1;
    }
#endif

#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148) || defined(CONFIG_BCM94908) || defined(CONFIG_BCM96858) || defined(CONFIG_BCM96836)
    /* check if it is zero padded or no 64K padding at all for backward compatiblity */
    if(whole_image && ( (wt.wfiFlags&WFI_FLAG_HAS_PMC) == 0) )
    {
        unsigned int *pImg  = (unsigned int*)string;
        unsigned char * pBuf = string;
        int block_start, block_end, sect_size, remain, block;
        
        if( *pImg == 0 && *(pImg+1) == 0 && *(pImg+2) == 0 && *(pImg+3) == 0 )
        {
           /* the first 64KB are for PMC in 631x8, need to preserve that for cfe/linux image update
           if it is not for PMC image update. */
           sect_size = flash_get_sector_size(0);
           block_start = 0;
           block_end = IMAGE_OFFSET/sect_size;
           remain = IMAGE_OFFSET%sect_size;

           for( block = block_start; block < block_end; block++ )
           {
               flash_read_buf(block, 0, pBuf, sect_size);
               pBuf += sect_size;
           }
         
           if( remain )
           {
              block = block_end;
               flash_read_buf(block, 0, pBuf, remain);
           }
        }
        else
        {
            /* does not have PMC at all, the input data starting from 64KB offset */
            if( (flash_get_flash_type() == FLASH_IFC_SPI) || (flash_get_flash_type() == FLASH_IFC_HS_SPI)  )
                flash_start_addr += IMAGE_OFFSET;
        }
    }
#endif

#if defined(DEBUG_FLASH)
    printk("kerSysBcmImageSet: flash_start_addr=0x%x string=%p len=%d whole_image=%d\n",
           flash_start_addr, string, size, whole_image);
#endif

    blk_start = flash_get_blk(flash_start_addr);

    if( blk_start < 0 )
        return( -1 );

    is_cfe_write = ((NVRAM_SECTOR == blk_start) &&
                    (size <= FLASH_LENGTH_BOOT_ROM));
#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148) || defined(CONFIG_BCM94908) || defined(CONFIG_BCM96858) || defined(CONFIG_BCM96836)
    /* if PMC image is included, it is also considered as CFE write */
    if( blk_start == 0 && size <= (FLASH_LENGTH_BOOT_ROM+IMAGE_OFFSET) )
        is_cfe_write = 1;
#endif

    /*
     * write image to flash memory.
     * In theory, all calls to flash_write_buf() must be done with
     * semflash held, so I added it here.  However, in reality, all
     * flash image writes are protected by flashImageMutex at a higher
     * level.
     */
    down(&semflash);


    // Once we have acquired the flash semaphore, we can
    // disable activity on other processor and also on local processor.
    // Need to disable interrupts so that RCU stall checker will not complain.
    if (!is_cfe_write && !should_yield)
    {
        stopOtherCpu();
        local_irq_save(flags);
    }

    local_bh_disable();
#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148)
    /* stall pmc if we are writing to the pmc flash address*/
    if( flash_start_addr == FLASH_BASE )
         StallPmc();
#endif
    do 
    {
        sect_size = flash_get_sector_size(blk_start);
        /* better to do read/modify/write for PMC code on 63138 at first 64K.
           so far SPI flash only has sector size up to 64KB. So we are ok for now */
        flash_sector_erase_int(blk_start);     // erase blk before flash

        if (sect_size > size) 
        {
            if (size & 1) 
                size++;
            sect_size = size;
        }

#if defined(CONFIG_BCM963268)
        if ((NVRAM_SECTOR == blk_start) && (wt.wfiFlags & WFI_FLAG_SUPPORTS_BTRM) && bcm_otp_is_boot_secure())
        {
            /* Upgrading a secure-boot 63268 SoC, and we are about to update the sector  */
            /* that contains the 3k nvram. Ensure the new security credentials make it   */
            /* into flash and the in-memory copy of nvram. Do this by making nvram valid */
            /* within the new image before writing it to flash */
            pnd = (PNVRAM_DATA)(string + NVRAM_DATA_OFFSET);
	    kerSysNvRamGet((char*)pnd, 1024, 0); /* do not change 1024 to be sizeof(NVRAM_DATA) */
            pnd->ulCheckSum = 0;
            pnd->ulCheckSum = crc32(CRC32_INIT_VALUE, pnd, sizeof(NVRAM_DATA));
         }
#endif

        if (flash_write_buf(blk_start, 0, string, sect_size) != sect_size) {
            break;
        }

        // check if we just wrote into the sector where the NVRAM is.
        // update our in-memory copy
        if (NVRAM_SECTOR == blk_start)
        {
            updateInMemNvramData(string+NVRAM_DATA_OFFSET, NVRAM_LENGTH, 0);
        }

        printk(".");
        blk_start++;
        string += sect_size;
        size -= sect_size; 

        if (should_yield)
        {
            local_bh_enable();
            yield();
            local_bh_disable();
        }
    } while (size > 0);

#if defined(CONFIG_BCM963138) || defined(CONFIG_BCM963148)
    /* stall pmc if we are writing to the pmc flash address*/
    if( flash_start_addr == FLASH_BASE )
         UnstallPmc();
#endif
    if (whole_image && savedSize > fInfo.flash_rootfs_start_offset)
    {
        // If flashing a whole image, erase to end of flash.
        int total_blks = flash_get_numsectors();
        while( blk_start < total_blks )
        {
            flash_sector_erase_int(blk_start);
            printk(".");
            blk_start++;

            if (should_yield)
            {
                local_bh_enable();
                yield();
                local_bh_disable();
            }
        }
    }

    up(&semflash);

    printk("\n\n");

    if (is_cfe_write || should_yield)
    {
        local_bh_enable();
    }

    if( size == 0 )
    {
        sts = 0;  // ok
    }
    else
    {
        /*
         * Even though we try to recover here, this is really bad because
         * we have stopped the other CPU and we cannot restart it.  So we
         * really should try hard to make sure flash writes will never fail.
         */
        printk(KERN_ERR "kerSysBcmImageSet: write failed at blk=%d\n",
                        blk_start);
        sts = blk_start;    // failed to flash this sector
        if (!is_cfe_write && !should_yield)
        {
            local_irq_restore(flags);
            local_bh_enable();
        }
    }

    return sts;
}


/***********************************************************************
 * Function Name: writeBootImageState
 * Description  : Persistently sets the state of an image update.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
static int writeBootImageState( int currState, int newState )
{
    int ret = -1;

    if((flash_type == FLASH_IFC_SPI) || (flash_type == FLASH_IFC_HS_SPI))
    {
        /* NOR flash */
        char *pShareBuf = NULL;

        if( (pShareBuf = getSharedBlks( fInfo.flash_scratch_pad_start_blk,
            fInfo.flash_scratch_pad_number_blk)) != NULL )
        {
            PSP_HEADER pHdr = (PSP_HEADER) pShareBuf;
            unsigned long *pBootImgState=(unsigned long *)&pHdr->NvramData2[0];

            /* The boot image state is stored as a word in flash memory where
             * the most significant three bytes are a "magic number" and the
             * least significant byte is the state constant.
             */
            if( (*pBootImgState & 0xffffff00) == (BLPARMS_MAGIC & 0xffffff00) )
            {
                *pBootImgState &= ~0x000000ff;
                *pBootImgState |= newState;

                ret = setSharedBlks(fInfo.flash_scratch_pad_start_blk,    
                    fInfo.flash_scratch_pad_number_blk,  pShareBuf);
            }

            retriedKfree(pShareBuf);
        }
    }
    else
    {
        /* NAND flash */
        printk("%s: no longer support NAND flash in kernel\n", __FUNCTION__);
    }

    return( ret );
}

/***********************************************************************
 * Function Name: readBootImageState
 * Description  : Reads the current boot image state from flash memory.
 * Returns      : state constant or -1 for failure
 ***********************************************************************/
static int readBootImageState( void )
{
    int ret = -1;

    if((flash_type == FLASH_IFC_SPI) || (flash_type == FLASH_IFC_HS_SPI))
    {
        /* NOR flash */
        char *pShareBuf = NULL;

        if( (pShareBuf = getSharedBlks( fInfo.flash_scratch_pad_start_blk,
            fInfo.flash_scratch_pad_number_blk)) != NULL )
        {
            PSP_HEADER pHdr = (PSP_HEADER) pShareBuf;
            unsigned long *pBootImgState=(unsigned long *)&pHdr->NvramData2[0];

            /* The boot image state is stored as a word in flash memory where
             * the most significant three bytes are a "magic number" and the
             * least significant byte is the state constant.
             */
            if( (*pBootImgState & 0xffffff00) == (BLPARMS_MAGIC & 0xffffff00) )
            {
                ret = *pBootImgState & 0x000000ff;
            }

            retriedKfree(pShareBuf);
        }
    }
    else
    {
        /* NAND flash */
        printk("%s: no longer support NAND flash in kernel\n", __FUNCTION__);
    }

    return( ret );
}

/***********************************************************************
 * Function Name: updateSequenceNumber
 * Description  : updates the sequence number on the specified partition
 *                to be the highest.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
static int updateSequenceNumber(int incSeqNumPart, int seqPart1, int seqPart2)
{
    int ret = -1;

    mutex_lock(&flashImageMutex);
    if((flash_type == FLASH_IFC_SPI) || (flash_type == FLASH_IFC_HS_SPI))
    {
        /* NOR flash */
        char *pShareBuf = NULL;
        PFILE_TAG pTag;
        int blk;

        pTag = kerSysUpdateTagSequenceNumber(incSeqNumPart);
        blk = *(int *) (pTag + 1);

        if ((pShareBuf = getSharedBlks(blk, 1)) != NULL)
        {
            memcpy(pShareBuf, pTag, sizeof(FILE_TAG));
            setSharedBlks(blk, 1, pShareBuf);
            retriedKfree(pShareBuf);
        }
    }
    else
    {
        /* NAND flash */
        printk("%s: no longer support NAND flash in kernel\n", __FUNCTION__);
    }
    mutex_unlock(&flashImageMutex);

    return( ret );
}

/***********************************************************************
 * Function Name: kerSysSetBootImageState
 * Description  : Persistently sets the state of an image update.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
int kerSysSetBootImageState( int newState )
{
    int ret = -1;
    int incSeqNumPart = -1;
    int writeImageState = 0;
    int currState = readBootImageState();
    int seq1 = kerSysGetSequenceNumber(1);
    int seq2 = kerSysGetSequenceNumber(2);

    /* Update the image state persistently using "new image" and "old image"
     * states.  Convert "partition" states to "new image" state for
     * compatibility with the non-OMCI image update.
     */
    mutex_lock(&spMutex);
    switch(newState)
    {
    case BOOT_SET_PART1_IMAGE:
        if( seq1 != -1 )
        {
            if( seq1 < seq2 )
                incSeqNumPart = 1;
            newState = BOOT_SET_NEW_IMAGE;
            writeImageState = 1;
        }
        break;

    case BOOT_SET_PART2_IMAGE:
        if( seq2 != -1 )
        {
            if( seq2 < seq1 )
                incSeqNumPart = 2;
            newState = BOOT_SET_NEW_IMAGE;
            writeImageState = 1;
        }
        break;

    case BOOT_SET_PART1_IMAGE_ONCE:
        if( seq1 != -1 )
        {
            if( seq1 < seq2 )
                incSeqNumPart = 1;
            newState = BOOT_SET_NEW_IMAGE_ONCE;
            writeImageState = 1;
        }
        break;

    case BOOT_SET_PART2_IMAGE_ONCE:
        if( seq2 != -1 )
        {
            if( seq2 < seq1 )
                incSeqNumPart = 2;
            newState = BOOT_SET_NEW_IMAGE_ONCE;
            writeImageState = 1;
        }
        break;

    case BOOT_SET_OLD_IMAGE:
    case BOOT_SET_NEW_IMAGE:
    case BOOT_SET_NEW_IMAGE_ONCE:
        /* The boot image state is stored as a word in flash memory where
         * the most significant three bytes are a "magic number" and the
         * least significant byte is the state constant.
         */
        if( currState == newState )
        {
            ret = 0;
        }
        else
        {
            writeImageState = 1;

            if(newState==BOOT_SET_NEW_IMAGE && currState==BOOT_SET_OLD_IMAGE)
            {
                /* The old (previous) image is being set as the new
                 * (current) image. Make sequence number of the old
                 * image the highest sequence number in order for it
                 * to become the new image.
                 */
                incSeqNumPart = 0;
            }
        }
        break;

    default:
        break;
    }

    if( writeImageState )
        ret = writeBootImageState(currState, newState);

    mutex_unlock(&spMutex);

    if( incSeqNumPart != -1 )
        updateSequenceNumber(incSeqNumPart, seq1, seq2);

    return( ret );
}

/***********************************************************************
 * Function Name: kerSysGetBootImageState
 * Description  : Gets the state of an image update from flash.
 * Returns      : state constant or -1 for failure
 ***********************************************************************/
int kerSysGetBootImageState( void )
{
    int ret = readBootImageState();

    if( ret != -1 )
    {
        int seq1 = kerSysGetSequenceNumber(1);
        int seq2 = kerSysGetSequenceNumber(2);

        switch(ret)
        {
        case BOOT_SET_NEW_IMAGE:
            if( seq1 == -1 || seq1 < seq2)
                ret = BOOT_SET_PART2_IMAGE;
            else
                ret = BOOT_SET_PART1_IMAGE;
            break;

        case BOOT_SET_NEW_IMAGE_ONCE:
            if( seq1 == -1 || seq1 < seq2)
                ret = BOOT_SET_PART2_IMAGE_ONCE;
            else
                ret = BOOT_SET_PART1_IMAGE_ONCE;
            break;

        case BOOT_SET_OLD_IMAGE:
            if( seq1 == -1 || seq1 > seq2)
                ret = BOOT_SET_PART2_IMAGE;
            else
                ret = BOOT_SET_PART1_IMAGE;
            break;

        default:
            ret = -1;
            break;
        }
    }

    return( ret );
}


// Must be called with flashImageMutex held
static PFILE_TAG getTagFromPartition(int imageNumber)
{
    // Define space for file tag structures for two partitions.  Make them static
    // so caller does not have to worry about allocation/deallocation.
    // Make sure they're big enough for the file tags plus an block number
    // (an integer) appended.
    static unsigned char sectAddr1[sizeof(FILE_TAG) + sizeof(int)];
    static unsigned char sectAddr2[sizeof(FILE_TAG) + sizeof(int)];

    long blk = 0;
    UINT32 crc;
    PFILE_TAG pTag = NULL;
    unsigned char *pBase = flash_get_memptr(0);
    unsigned char *pSectAddr = NULL;

    unsigned int reserverdBytersAuxfs = flash_get_reserved_bytes_auxfs();
    unsigned int sectSize = (unsigned int) flash_get_sector_size(0);
    unsigned int offset;

    /* The image tag for the first image is always after the boot loader.
     * The image tag for the second image, if it exists, is at one half
     * of the flash size.
     */
    if( imageNumber == 1 )
    {
        // Get the flash info and block number for parition 1 at the base of the flash
        FLASH_ADDR_INFO flash_info;

        kerSysFlashAddrInfoGet(&flash_info);
        blk = flash_get_blk((long)(pBase+flash_info.flash_rootfs_start_offset));
        pSectAddr = sectAddr1;
    }
    else if( imageNumber == 2 )
    {
        // Get block number for partition2 at middle of the device (not counting space for aux
        // file system).
        offset = ((flash_get_total_size()-reserverdBytersAuxfs) / 2);

        /* make sure offset is on sector boundary, image starts on sector boundary */
        if( offset % sectSize )
            offset = ((offset/sectSize)+1)*sectSize;
        blk = flash_get_blk((long) (pBase + offset + IMAGE_OFFSET));

        pSectAddr = sectAddr2;
    }
    
    // Now that you have a block number, use it to read in the file tag
    if( blk )
    {
        int *pn;    // Use to append block number at back of file tag
        
        // Clear out space for file tag structures
        memset(pSectAddr, 0x00, sizeof(FILE_TAG));
        
        // Get file tag
        flash_read_buf((unsigned short) blk, 0, pSectAddr, sizeof(FILE_TAG));
        
        // Figure out CRC of file tag so we can check it below
        crc = CRC32_INIT_VALUE;
        crc = genCrc32(pSectAddr, (UINT32)TAG_LEN-TOKEN_LEN, crc);
        
        // Get ready to return file tag pointer
        pTag = (PFILE_TAG) pSectAddr;
        
        // Append block number after file tag
        pn = (int *) (pTag + 1);
        *pn = blk;
        
        // One final check - if the file tag CRC is not OK, return NULL instead
        if (crc != (UINT32)(*(UINT32*)(pTag->tagValidationToken)))
            pTag = NULL;
    }
    
    // All done - return file tag pointer
    return( pTag );
}

// must be called with flashImageMutex held
static int getPartitionFromTag( PFILE_TAG pTag )
{
    int ret = 0;

    if( pTag )
    {
        PFILE_TAG pTag1 = getTagFromPartition(1);
        PFILE_TAG pTag2 = getTagFromPartition(2);
        int sequence = simple_strtoul(pTag->imageSequence,  NULL, 10);
        int sequence1 = (pTag1) ? simple_strtoul(pTag1->imageSequence, NULL, 10)
            : -1;
        int sequence2 = (pTag2) ? simple_strtoul(pTag2->imageSequence, NULL, 10)
            : -1;

        if( pTag1 && sequence == sequence1 )
            ret = 1;
        else
            if( pTag2 && sequence == sequence2 )
                ret = 2;
    }

    return( ret );
}

// must be called with flashImageMutex held
static PFILE_TAG getBootImageTag(void)
{
    static int displayFsAddr = 1;
    PFILE_TAG pTag = NULL;
    PFILE_TAG pTag1 = getTagFromPartition(1);
    PFILE_TAG pTag2 = getTagFromPartition(2);

    BCM_ASSERT_HAS_MUTEX_C(&flashImageMutex);

    if( pTag1 && pTag2 )
    {
        /* Two images are flashed. */
        int sequence1 = simple_strtoul(pTag1->imageSequence, NULL, 10);
        int sequence2 = simple_strtoul(pTag2->imageSequence, NULL, 10);
        int imgid = 0;

        kerSysBlParmsGetInt(BOOTED_IMAGE_ID_NAME, &imgid);
        if( imgid == BOOTED_OLD_IMAGE )
            pTag = (sequence2 < sequence1) ? pTag2 : pTag1;
        else
            pTag = (sequence2 > sequence1) ? pTag2 : pTag1;
    }
    else
        /* One image is flashed. */
        pTag = (pTag2) ? pTag2 : pTag1;

    if( pTag && displayFsAddr )
    {
        displayFsAddr = 0;
        printk("File system address: 0x%8.8lx\n",
            simple_strtoul(pTag->rootfsAddress, NULL, 10) + BOOT_OFFSET + IMAGE_OFFSET);
    }

    return( pTag );
}

// Must be called with flashImageMutex held
static void UpdateImageSequenceNumber( unsigned char *imageSequence )
{
    int newImageSequence = 0;
    PFILE_TAG pTag = getTagFromPartition(1);

    if( pTag )
        newImageSequence = simple_strtoul(pTag->imageSequence, NULL, 10);

    pTag = getTagFromPartition(2);
    if(pTag && simple_strtoul(pTag->imageSequence, NULL, 10) > newImageSequence)
        newImageSequence = simple_strtoul(pTag->imageSequence, NULL, 10);

    newImageSequence++;
    sprintf(imageSequence, "%d", newImageSequence);
}

/* Must be called with flashImageMutex held */
int flashFsKernelImage( unsigned char *imagePtr, int imageLen,
    int flashPartition, int *numPartitions )
{

    int status = 0;
    PFILE_TAG pTag = (PFILE_TAG) imagePtr;
    int rootfsAddr = simple_strtoul(pTag->rootfsAddress, NULL, 10);
    int kernelAddr = simple_strtoul(pTag->kernelAddress, NULL, 10);
    int dtbAddr = 0;
    int checkDtb = 0;
    char *tagFs = imagePtr;
    unsigned int baseAddr = (unsigned int) (uintptr_t)flash_get_memptr(0);
    unsigned int totalSize = (unsigned int) flash_get_total_size();
    unsigned int sectSize = (unsigned int) flash_get_sector_size(0);
    unsigned int reservedBytesAtEnd;
    unsigned int reserverdBytersAuxfs;
    unsigned int availableSizeOneImg;
    unsigned int reserveForTwoImages;
    unsigned int availableSizeTwoImgs;
    unsigned int newImgSize = simple_strtoul(pTag->rootfsLen, NULL, 10) +
        simple_strtoul(pTag->kernelLen, NULL, 10);
    PFILE_TAG pCurTag;
    int nCurPartition;
    int should_yield;
    UINT32 crc;
    unsigned int curImgSize = 0;
    unsigned int rootfsOffset = (unsigned int) rootfsAddr - IMAGE_BASE - TAG_LEN + IMAGE_OFFSET;
    FLASH_ADDR_INFO flash_info;
    NVRAM_DATA *pNvramData;

    mutex_lock(&flashImageMutex);
    
    pCurTag = getBootImageTag();
    nCurPartition = getPartitionFromTag( pCurTag );

    if (NULL == (pNvramData = readNvramData()))
    {
        mutex_unlock(&flashImageMutex);
        return -ENOMEM;
    }

    /* flashPartition == 0 default option meaning flash to the inactive partition */
    if( flashPartition == 0 )
        flashPartition = (nCurPartition == 2) ? 1:2;

    should_yield = (flashPartition == nCurPartition) ? 0 : 1;


    checkDtb = simple_strtoul(pTag->tagVersion, NULL, 10) >= BCM_TAG_VER_DTB ? 1 : 0;
    if( checkDtb )
    {
        dtbAddr = simple_strtoul(pTag->dtbAddress, NULL, 10);
        if( dtbAddr == 0 )
            checkDtb = 0;
    }

    /* update total image size if there is dtb at the end */
    if( checkDtb )
        newImgSize += simple_strtoul(pTag->dtbLen, NULL, 10);

    kerSysFlashAddrInfoGet(&flash_info);
    if( rootfsOffset < flash_info.flash_rootfs_start_offset )
    {
        // Increase rootfs and kernel addresses by the difference between
        // rootfs offset and what it needs to be.
        rootfsAddr += flash_info.flash_rootfs_start_offset - rootfsOffset;
        kernelAddr += flash_info.flash_rootfs_start_offset - rootfsOffset;
        sprintf(pTag->rootfsAddress,"%u", (unsigned int) rootfsAddr);
        sprintf(pTag->kernelAddress,"%u", (unsigned int) kernelAddr);
        if( checkDtb )
        {
            dtbAddr += flash_info.flash_rootfs_start_offset - rootfsOffset;
            sprintf(pTag->dtbAddress,"%u", (unsigned int) dtbAddr);
        }

        crc = CRC32_INIT_VALUE;
        crc = genCrc32((unsigned char *)pTag, (UINT32)TAG_LEN-TOKEN_LEN, crc);
        *(unsigned int *) &pTag->tagValidationToken[0] = crc;
    }

    rootfsAddr += BOOT_OFFSET+IMAGE_OFFSET;
    kernelAddr += BOOT_OFFSET+IMAGE_OFFSET;
    if( checkDtb )
        dtbAddr += BOOT_OFFSET+IMAGE_OFFSET;

    reservedBytesAtEnd = flash_get_reserved_bytes_at_end(&flash_info);
    reserverdBytersAuxfs = flash_get_reserved_bytes_auxfs();
    availableSizeOneImg = totalSize - ((unsigned int) rootfsAddr - baseAddr) -
        reservedBytesAtEnd- reserverdBytersAuxfs;  
        
    reserveForTwoImages =
        (flash_info.flash_rootfs_start_offset > reservedBytesAtEnd)
        ? flash_info.flash_rootfs_start_offset : reservedBytesAtEnd;
    availableSizeTwoImgs = ((totalSize-reserverdBytersAuxfs)/ 2) - reserveForTwoImages - sectSize;

    printk("availableSizeOneImage=%dKB availableSizeTwoImgs=%dKB reserverdBytersAuxfs=%dKB reserve=%dKB\n",
        availableSizeOneImg/1024, availableSizeTwoImgs/1024, reserverdBytersAuxfs/1024, reserveForTwoImages/1024);
       
    if( pCurTag )
    {
        curImgSize = simple_strtoul(pCurTag->rootfsLen, NULL, 10) +
            simple_strtoul(pCurTag->kernelLen, NULL, 10);
        if( simple_strtoul(pCurTag->tagVersion, NULL, 10) >= BCM_TAG_VER_DTB )
            curImgSize += simple_strtoul(pCurTag->dtbLen, NULL, 10);
    }

    if( newImgSize > availableSizeOneImg)
    {
        printk("Illegal image size %d.  Image size must not be greater "
            "than %d.\n", newImgSize, availableSizeOneImg);
        kfree(pNvramData);
        mutex_unlock(&flashImageMutex);
        return -1;
    }

    *numPartitions = (curImgSize <= availableSizeTwoImgs &&
         newImgSize <= availableSizeTwoImgs &&
         flashPartition != nCurPartition) ? 2 : 1;

    // If the current image fits in half the flash space and the new
    // image to flash also fits in half the flash space, then flash it
    // in the partition that is not currently being used to boot from.
    if( curImgSize <= availableSizeTwoImgs &&
        newImgSize <= availableSizeTwoImgs &&
        ((nCurPartition == 1 && flashPartition != 1) || flashPartition == 2) )
    {
        // Update rootfsAddr to point to the second boot partition.
        int offset = ((totalSize-reserverdBytersAuxfs) / 2);

        /* make sure offset is on sector boundary, image starts on sector boundary */
        if( offset % sectSize )
            offset = ((offset/sectSize)+1)*sectSize;
        offset += TAG_LEN;

        if( checkDtb )
        {
            sprintf(((PFILE_TAG) tagFs)->dtbAddress, "%u",
                    (unsigned int) IMAGE_BASE + offset + (dtbAddr-rootfsAddr));
            dtbAddr = baseAddr + offset + (dtbAddr - rootfsAddr) + IMAGE_OFFSET;
        }
   
        sprintf(((PFILE_TAG) tagFs)->kernelAddress, "%u",
            (unsigned int) IMAGE_BASE + offset + (kernelAddr - rootfsAddr));
        kernelAddr = baseAddr + offset + (kernelAddr - rootfsAddr) + IMAGE_OFFSET;

        sprintf(((PFILE_TAG) tagFs)->rootfsAddress, "%u",
            (unsigned int) IMAGE_BASE + offset);
        rootfsAddr = baseAddr + offset + IMAGE_OFFSET;
    }

    if( newImgSize > availableSizeTwoImgs ) 
    {
        printk("new image size large than partition size, force to single partition!\n");
        flashPartition = 1;
        *numPartitions = 1;
        should_yield = 0;
    }

    UpdateImageSequenceNumber( ((PFILE_TAG) tagFs)->imageSequence );
    crc = CRC32_INIT_VALUE;
    crc = genCrc32((unsigned char *)tagFs, (UINT32)TAG_LEN-TOKEN_LEN, crc);
    *(unsigned int *) &((PFILE_TAG) tagFs)->tagValidationToken[0] = crc;

    
    // Now, perform the actual flash write
    if( (status = kerSysBcmImageSet((rootfsAddr-TAG_LEN), tagFs,
        TAG_LEN + newImgSize, should_yield)) != 0 )
    {
        printk("Failed to flash root file system. Error: %d\n", status);
        kfree(pNvramData);
        mutex_unlock(&flashImageMutex);
        return status;
    }

    // Free buffers
    kfree(pNvramData);
    mutex_unlock(&flashImageMutex);

    return(status);
}


#ifdef __LITTLE_ENDIAN__
#define je16_to_cpu(x) (le16_to_cpu(x))
#else
#define je16_to_cpu(x) (be16_to_cpu(x))
#endif

/***********************************************************************
 * Function Name: scan_partition
 * Description  : scans mtd partition to determine type
 * Returns      : none, JFFS2 or (pure) UBI image type
 ***********************************************************************/
static int image_type(struct mtd_info * mtd)
{
    long i;
    size_t retlen;
    unsigned char buf[4];
#if defined(CONFIG_MTD_NAND)
    UINT32 * ec_magic = (UINT32*)buf;
#endif
    UINT16 * pdir_magic = (UINT16*)buf;

    for( i = 0; i < mtd->size; i += mtd->erasesize )
    {
        if(mtd_read(mtd, 0, 4, &retlen, buf) < 0)
            continue;

        if(je16_to_cpu(*pdir_magic) == JFFS2_MAGIC_BITMASK)
            return(JFFS2_IMAGE);
#if defined(CONFIG_MTD_NAND)
        if(be32_to_cpu(*ec_magic) == UBI_EC_HDR_MAGIC)
            return(UBI_IMAGE);
#endif
    }

    return(0);
}


void clearImageVersion(void)
{
    memset(&imageVersions, 0, sizeof(imageVersions));
    return;
}

int kerSysGetImageVersion( int imageNumber, char *verStr, int verStrSize)
{
    int ret = 0; /* zero bytes copied so far */

    if( !((imageNumber == 1 && imageVersions[0][0] != '\0') ||
        (imageNumber == 2 && imageVersions[1][0] != '\0')) )
    {
        /* get up to IMAGE_VERSION_MAX_SIZE and save it in imageVersions[][] */
        unsigned int rootfs_ofs;

        memset(imageVersions[imageNumber - 1], 0, IMAGE_VERSION_MAX_SIZE);
        
        if( kerSysBlParmsGetInt(NAND_RFS_OFS_NAME, (int *) &rootfs_ofs) == -1 )
        {
            /* NOR Flash */
            PFILE_TAG pTag = NULL;

            if( imageNumber == 1 )
                pTag = getTagFromPartition(1);
            else
                if( imageNumber == 2 )
                    pTag = getTagFromPartition(2);

            if( pTag )
            {
                /* Save version string for subsequent calls to this function. 
                 MAX length  IMAGE_VER_LEN = 32 bytes */
                memcpy(imageVersions[imageNumber - 1], pTag->imageVersion, IMAGE_VER_LEN);
            }
        }
        else
        {
            /* NAND Flash */
            NVRAM_DATA *pNvramData;

            if( (imageNumber == 1 || imageNumber == 2) &&
                (pNvramData = readNvramData()) != NULL )
            {
                char *pImgVerFileName = NULL;

                mm_segment_t fs;
                struct file *fp;
                int updatePart, getFromCurPart;
                int type = 0;

                // updatePart is the partition number that is not booted
                // getFromCurPart is 1 to retrieve info from the booted partition
                updatePart =
                    (rootfs_ofs==pNvramData->ulNandPartOfsKb[NP_ROOTFS_1])
                    ? 2 : 1;
                getFromCurPart = (updatePart == imageNumber) ? 0 : 1;

                fs = get_fs();
                set_fs(get_ds());
                if( getFromCurPart == 0 )
                {
                    struct mtd_info *mtd;
                    pImgVerFileName = "/mnt/fs_update/" IMAGE_VERSION_FILE_NAME;
                    if( !IS_ERR_OR_NULL(mtd = get_mtd_device_nm("bootfs_update")) )
                    {
                        sys_mount("mtd:bootfs_update", "/mnt/fs_update","jffs2",MS_RDONLY,NULL);
                        put_mtd_device(mtd);
                    }
                    else
                    {
                        if( !IS_ERR_OR_NULL(mtd = get_mtd_device_nm("rootfs_update")) )
                        {
                            type = image_type(mtd);

                            if (type == JFFS2_IMAGE)
                                sys_mount("mtd:rootfs_update", "/mnt/fs_update","jffs2",MS_RDONLY,NULL);
#if defined(CONFIG_MTD_NAND)
                            if (type == UBI_IMAGE)
                            {
                                char * envp[] = { NULL };
                                char * argv[] = {"/bin/ubiattach", "-m1", "-d2", NULL};
                                call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
                                sys_mount("ubi2:rootfs_ubifs", "/mnt/fs_update","ubifs",MS_RDONLY,NULL);
                            }
#endif
                            put_mtd_device(mtd);
                        }
                    }
                }
                else
                    pImgVerFileName = IMAGE_VERSION_FILE_NAME;

                fp = filp_open(pImgVerFileName, O_RDONLY, 0);
                if( !IS_ERR(fp) )
                {
                    /* File open successful, read version string from file. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
                    if(fp->f_op && fp->f_op->read)
#endif
                    {
                        fp->f_pos = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
                        ret = fp->f_op->read(fp, (void *) imageVersions[imageNumber - 1], 
#else
                        ret = vfs_read(fp, (void *) imageVersions[imageNumber - 1], 
#endif
                            IMAGE_VERSION_MAX_SIZE,
                            &fp->f_pos);

                        if (ret > 0)
                        {
                            int i;
                            for (i = 0; i < ret; i ++)
                            {
                                if (imageVersions[imageNumber - 1][i] == 0xa)//line feed
                                {
                                    imageVersions[imageNumber - 1][i] = '\0';//end
                                    ret = i+1;
                                    break;
                                }
                            }
                        }
                    }
                    
                    filp_close(fp, NULL);
                }

                if( getFromCurPart == 0 )
                {
                    sys_umount("/mnt/fs_update", 0);
#if defined(CONFIG_MTD_NAND)
                    if (type == UBI_IMAGE)
                    {
                        char * envp[] = { NULL };
                        char * argv[] = {"/bin/ubidetach", "-d2", NULL};
                        call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
                    }
#endif
                }

                set_fs(fs);
                kfree(pNvramData);
            }
        }
    }
    
    /* copy the first verStrSize bytes of the stored version to the caller's buffer */
    if( verStrSize > IMAGE_VERSION_MAX_SIZE )
        ret = IMAGE_VERSION_MAX_SIZE;
    else
        ret = verStrSize;
    memcpy(verStr, imageVersions[imageNumber - 1], ret);

    return( ret );
}

PFILE_TAG kerSysUpdateTagSequenceNumber(int imageNumber)
{
    PFILE_TAG pTag = NULL;
    UINT32 crc;

    switch( imageNumber )
    {
    case 0:
        pTag = getBootImageTag();
        break;

    case 1:
        pTag = getTagFromPartition(1);
        break;

    case 2:
        pTag = getTagFromPartition(2);
        break;

    default:
        break;
    }

    if( pTag )
    {
        UpdateImageSequenceNumber( pTag->imageSequence );
        crc = CRC32_INIT_VALUE;
        crc = genCrc32((unsigned char *)pTag, (UINT32)TAG_LEN-TOKEN_LEN, crc);
        *(unsigned int *) &pTag->tagValidationToken[0] = crc;
    }

    return(pTag);
}

int kerSysGetSequenceNumber(int imageNumber)
{
    int seqNumber = -1;
    unsigned int rootfs_ofs;
    if( kerSysBlParmsGetInt(NAND_RFS_OFS_NAME, (int *) &rootfs_ofs) == -1 )
    {
        /* NOR Flash */
        PFILE_TAG pTag = NULL;

        switch( imageNumber )
        {
        case 0:
            pTag = getBootImageTag();
            break;

        case 1:
            pTag = getTagFromPartition(1);
            break;

        case 2:
            pTag = getTagFromPartition(2);
            break;

        default:
            break;
        }

        if( pTag )
            seqNumber= simple_strtoul(pTag->imageSequence, NULL, 10);
    }
    else
    {
        /* NAND Flash */
        NVRAM_DATA *pNvramData;

        printk("checking %s partition for sequence number\n", (imageNumber == 1) ? "first" : "second");

        if( (pNvramData = readNvramData()) != NULL )
        {
            char fname[] = NAND_CFE_RAM_NAME;
            char cferam_buf[32], cferam_fmt[32]; 
            int i;

            mm_segment_t fs;
            struct file *fp;
            int updatePart, getFromCurPart;

#if !defined(CONFIG_BCM960333) && !defined(CONFIG_BCM947189)
            /* If full secure boot is in play, the CFE RAM file is the encrypted version */
            if (bcm_otp_is_boot_secure())
               strcpy(fname, NAND_CFE_RAM_SECBT_NAME);
#if defined(CONFIG_BCM94908) || defined(CONFIG_BCM96858) || defined(CONFIG_BCM96836) || defined(CONFIG_BCM963158)
            else
            {
               if (bcm_otp_is_boot_mfg_secure())
                  strcpy(fname, NAND_CFE_RAM_SECBT_MFG_NAME);
            }
#endif
#endif

            // updatePart is the partition number that is not booted
            // getFromCurPart is 1 to retrieive info from the booted partition
            updatePart = (rootfs_ofs==pNvramData->ulNandPartOfsKb[NP_ROOTFS_1])
                ? 2 : 1;
            getFromCurPart = (updatePart == imageNumber) ? 0 : 1;

            fs = get_fs();
            set_fs(get_ds());
            if( getFromCurPart == 0 )
            {
                struct mtd_info *mtd;
                strcpy(cferam_fmt, "/mnt/fs_update/");
                mtd = get_mtd_device_nm("bootfs_update");
                if( !IS_ERR_OR_NULL(mtd) )
                {
                    sys_mount("mtd:bootfs_update", "/mnt/fs_update","jffs2",MS_RDONLY,NULL);
                    put_mtd_device(mtd);
                }
                else
                    sys_mount("mtd:rootfs_update", "/mnt/fs_update","jffs2",MS_RDONLY,NULL);
            }
            else
            {
                struct mtd_info *mtd;
                mtd = get_mtd_device_nm("bootfs");
                if( !IS_ERR_OR_NULL(mtd) )
                {
                    strcpy(cferam_fmt, "/bootfs/");
                    put_mtd_device(mtd);
                }
                else
                    strcpy(cferam_fmt, "/");
            }

            /* Find the sequence number of the specified partition. */
            fname[strlen(fname) - 3] = '\0'; /* remove last three chars */
            strcat(cferam_fmt, fname);
            strcat(cferam_fmt, "%3.3d");

            for( i = 0; i < 1000; i++ )
            {
                sprintf(cferam_buf, cferam_fmt, i);
                fp = filp_open(cferam_buf, O_RDONLY, 0);
                if (!IS_ERR(fp) )
                {
                    filp_close(fp, NULL);

                    /* Seqence number found. */
                    seqNumber = i;
                    break;
                }
            }

            if( getFromCurPart == 0 )
                sys_umount("/mnt/fs_update", 0);

            set_fs(fs);
            kfree(pNvramData);
        }
    }

    return(seqNumber);
}

int getBootedValue(int getBootedPartition)
{
    static int s_bootedPartition = -1;
    int ret = -1;
    int imgId = -1;

    kerSysBlParmsGetInt(BOOTED_IMAGE_ID_NAME, &imgId);

    /* The boot loader parameter will only be "new image", "old image" or "only
     * image" in order to be compatible with non-OMCI image update. If the
     * booted partition is requested, convert this boot type to partition type.
     */
    if( imgId != -1 )
    {
        if( getBootedPartition )
        {
            if( s_bootedPartition != -1 )
                ret = s_bootedPartition;
            else
            {
                /* Get booted partition. */
                int seq1 = kerSysGetSequenceNumber(1);
                int seq2 = kerSysGetSequenceNumber(2);

                switch( imgId )
                {
                case BOOTED_NEW_IMAGE:
                    if( seq1 == -1 || seq2 > seq1 )
                        ret = BOOTED_PART2_IMAGE;
                    else
                        if( seq2 == -1 || seq1 >= seq2 )
                            ret = BOOTED_PART1_IMAGE;
                    break;

                case BOOTED_OLD_IMAGE:
                    if( seq1 == -1 || seq2 < seq1 )
                        ret = BOOTED_PART2_IMAGE;
                    else
                        if( seq2 == -1 || seq1 <= seq2 )
                            ret = BOOTED_PART1_IMAGE;
                    break;

                case BOOTED_ONLY_IMAGE:
                    ret = (seq1 == -1) ? BOOTED_PART2_IMAGE : BOOTED_PART1_IMAGE;
                    break;

                default:
                    break;
                }

                s_bootedPartition = ret;
            }
        }
        else
            ret = imgId;
    }

    return( ret );
}

#if !defined(CONFIG_BRCM_IKOS)
PFILE_TAG kerSysImageTagGet(void)
{
    PFILE_TAG tag;

    mutex_lock(&flashImageMutex);
    tag = getBootImageTag();
    mutex_unlock(&flashImageMutex);

    return tag;
}
#else
PFILE_TAG kerSysImageTagGet(void)
{
    return( (PFILE_TAG) (FLASH_BASE + FLASH_LENGTH_BOOT_ROM));
}
#endif

int setUserNvRam(char *string, int strLen, int offset) 
{
    int ret = 0;
    NVRAM_DATA * pNvramData;

    /*
     * Note: even though NVRAM access is protected by
     * flashImageMutex at the kernel level, this protection will
     * not work if two userspaces processes use ioctls to get
     * NVRAM data, modify it, and then use this ioctl to write
     * NVRAM data.  This seems like an unlikely scenario.
     */
    mutex_lock(&flashImageMutex);
    if (NULL == (pNvramData = readNvramData()))
    {
        mutex_unlock(&flashImageMutex);
        return -ENOMEM;
    }

    if ( !strncmp(string, "WLANFEATURE", 11 ) ) { //Wlan Data data
        pNvramData->wlanParams[NVRAM_WLAN_PARAMS_LEN-1]= *(unsigned char *)(string+11);
        writeNvramDataCrcLocked(pNvramData);
    }
    else if ( !strncmp(string, "WLANDATA", 8 ) ) { //Wlan Data data
        int t_strlen=strLen-8;
        int nm=_get_wl_nandmanufacture();

        if(nm<WLAN_MFG_PARTITION_HASSIZE) {
            if(t_strlen>NVRAM_WLAN_PARAMS_LEN-1)
                t_strlen=NVRAM_WLAN_PARAMS_LEN-1;
                memset((char *)pNvramData + ((size_t) &((NVRAM_DATA *)0)->wlanParams),
                    0, sizeof(pNvramData->wlanParams)-1 );
                memcpy( (char *)pNvramData + ((size_t) &((NVRAM_DATA *)0)->wlanParams),
                    string+8, t_strlen);
                writeNvramDataCrcLocked(pNvramData);
        } else {
            ret =_wlsrom_write_file(WL_SROM_CUSTOMER_FILE,(string+8),t_strlen);
            ret |=_wlsrom_write_file(WL_SROM_DEFAULT_FILE,(string+8),t_strlen);
            if(ret!=0) 
                printk("writing wl_srom file error!\n");
        }
    } else {
        // assumes the user has calculated the crc in the nvram struct
        ret = kerSysNvRamSet(string, strLen, offset);
    }
    mutex_unlock(&flashImageMutex);
    kfree(pNvramData);

    return ret;
}


/*
 * Common function used by BCM_IMAGE_CFE and BCM_IMAGE_WHOLE ioctls.
 * This function will acquire the flashImageMutex
 *
 * @return 0 on success, -1 on failure.
 */
int commonImageWrite(int flash_start_addr, char *string, int size,
    int *pnoReboot, int partition)
{
    NVRAM_DATA * pNvramDataOrig;
    NVRAM_DATA * pNvramDataNew=NULL;
    int ret = 0;

#if defined(CONFIG_BCM960333)
    disablePLCWatchdog();
#endif

    mutex_lock(&flashImageMutex);

    // Get a copy of the nvram before we do the image write operation
    if (NULL != (pNvramDataOrig = readNvramData()))
    {
        unsigned int rootfs_ofs;

        if( kerSysBlParmsGetInt(NAND_RFS_OFS_NAME, (int *) &rootfs_ofs) == -1 )
        {
            /* NOR flash */
            ret = kerSysBcmImageSet(flash_start_addr, string, size, 0);
            /* NOR whole image update always force to reboot as it wipe out whole flash
             * and flash the image on partion one */ 
            if( pnoReboot )
                *pnoReboot = 0;
        }
        else
        {
            /* NAND flash */
            printk("%s: no longer support NAND flash in kernel\n", __FUNCTION__);
            return -1;
        }

        /*
         * After the image is written, check the nvram.
         * If nvram is bad, write back the original nvram.
         */
        pNvramDataNew = readNvramData();
        if ((0 != ret) ||
            (NULL == pNvramDataNew) ||
            (BpSetBoardId(pNvramDataNew->szBoardId) != BP_SUCCESS)
#if defined (CONFIG_BCM_VOICE_SUPPORT)
            || (BpSetVoiceBoardId(pNvramDataNew->szVoiceBoardId) != BP_SUCCESS)
#endif
            )
        {
            // we expect this path to be taken.  When a CFE or whole image
            // is written, it typically does not have a valid nvram block
            // in the image.  We detect that condition here and restore
            // the previous nvram settings.  Don't print out warning here.
            writeNvramDataCrcLocked(pNvramDataOrig);

            // don't modify ret, it is return value from kerSysBcmImageSet
        }
    }
    else
    {
        ret = -1;
    }

    mutex_unlock(&flashImageMutex);

    if (pNvramDataOrig)
        kfree(pNvramDataOrig);
    if (pNvramDataNew)
        kfree(pNvramDataNew);

    /* Clean up image version. */
    if (ret == 0)
    {
        memset(&imageVersions, 0, sizeof(imageVersions));
    }

    return ret;
}

#if defined(CONFIG_MTD_NAND)

static struct mtd_partition bcm63XX_nand_parts[] =
{
    { name: "rootfs",        offset: 0,           size: 0 },
    { name: "rootfs_update", offset: 0,           size: 0 },
    { name: "data",          offset: 0,           size: 0 },
    { name: "nvram",         offset: 0,           size: 0 },
    { name: "image",         offset: 0,           size: 0 },
    { name: "image_update",  offset: 0,           size: 0 },
    { name: "rootfs_data",   offset: 0,           size: 0 },
    { name: "rootfs_update_data", offset: 0,      size: 0 },
    { name: "dummy2",        offset: 0,           size: 0 },
    { name: "dummy3",        offset: 0,           size: 0 },
    { name: "dummy4",        offset: 0,           size: 0 },
    { name: "dummy5",        offset: 0,           size: 0 },
    { name: "dummy6",        offset: 0,           size: 0 },
    { name: NULL,            offset: 0,           size: 0 }
};

#if 0
static char* misc_mtd_partition_names[BCM_MAX_EXTRA_PARTITIONS] =
{
    "misc1",
    "misc2",
    "misc3",
    NULL,
};


static unsigned int read_blk(unsigned char * start, unsigned int block, unsigned int offset, unsigned int blk_size, unsigned char * buf, unsigned int amount, void * mtdi, int mtd_fd)
{
    struct mtd_info* mtd = mtdi;
    size_t retlen;

    mtd->_read(mtd, (block * blk_size) + offset, amount, &retlen, buf + offset);

    return(retlen == amount);
}


static int is_split_partition(struct mtd_info* mtd, unsigned long offset, unsigned long size, unsigned long *split_offset)
{
    uint8_t buf[0x100];
    size_t retlen;
    struct ubi_ec_hdr ec;

    *split_offset = offset;
    while (mtd->_block_isbad(mtd, *split_offset))
        *split_offset += mtd->erasesize;

    // check first CFERAM block to see if it's UBI, and if so it is new image format
    mtd->_read(mtd, *split_offset, UBI_EC_HDR_SIZE, &retlen, (void*)&ec);

    if (be32_to_cpu(ec.magic) == UBI_EC_HDR_MAGIC) {
        unsigned char * buf = kmalloc(mtd->erasesize, GFP_KERNEL);
        char squash;
        int try, ret = UBIFS_FS;

        if (buf)
        {
            for (try = 0; try < 2; try++)
            {
                if ( (parse_ubi(0, buf, *split_offset / mtd->erasesize, (offset + size) / mtd->erasesize, mtd->erasesize, (try ? VOLID_METADATA_COPY : VOLID_METADATA), "squash", &squash, 0, 0, read_blk, 0, 0, mtd, 0) == 1) && (squash == '1') )
                {
                    ret = SQUBI_FS;
                    break;
                }
            }
        }

        if (buf)
            kfree(buf);

        return(ret);
    }

    /* Search RootFS partion for split marker.
     * Marker is located in the last 0x100 bytes of the last BootFS Erase Block
     * If marker is found, we have separate Boot and Root Partitions.
     */
    for (*split_offset = offset + mtd->erasesize; *split_offset <= offset + size; *split_offset += mtd->erasesize) {
        if (mtd->_block_isbad(mtd, *split_offset - mtd->erasesize)) {
            continue;
        }

        mtd->_read(mtd, *split_offset - 0x100, 0x100, &retlen, buf);

        if (!strncmp(BCM_BCMFS_TAG, buf, strlen(BCM_BCMFS_TAG))) {
            if (!strncmp(BCM_BCMFS_TYPE_SQUBIFS, &buf[strlen(BCM_BCMFS_TAG)], strlen(BCM_BCMFS_TYPE_SQUBIFS))) {
                return(SQSPLIT_FS);
            }  /* Note: determining SQSPLIT must be done before UBIFS!! Why? pls refer to clarification in the define of 'BCM_BCMFS_TYPE_SQUBIFS'*/
            else if (!strncmp(BCM_BCMFS_TYPE_UBIFS, &buf[strlen(BCM_BCMFS_TAG)], strlen(BCM_BCMFS_TYPE_UBIFS))) {
                return(SPLIT_FS);
            }
        }
    }
    return(JFFS2_FS);
}
#endif

int setup_mtd_parts(struct mtd_info* mtd)
{
#if 1
    static NVRAM_DATA nvram;
    unsigned long rootfs_ofs, update_ofs, high_bank_ofs, bank_size;
    unsigned long num_blks, bank_blks;
    struct mtd_info *mtdUbi;
    char mtdUbiArg[16];
    int nr_parts = 8;
    int is_new_fs = 0;

    /* The CFE boot loader saved the offset of the partition from which
     * the Linux image was loaded.
     */
    kerSysBlParmsGetInt(NAND_RFS_OFS_NAME, (int *) &rootfs_ofs);
    kerSysNvRamGet((char *)&nvram, sizeof(nvram), 0);
    kerSysBlParmsGetInt(NAND_NEW_UBI_FS,&is_new_fs);
    PRINTK("rootfs_ofs=0x%8.8lx\n", rootfs_ofs);

    num_blks = mtd->size >> 17; /* 2^17 = 128k */
    switch (num_blks) {
        case 1024: bank_blks = 491; break;
        case 2048: bank_blks = 1003; break;
        case 4096: bank_blks = 2027; break;
        default: bank_blks = 0;
    }

    if (!bank_blks || mtd->erasesize != 128*1024) {
        printk("Unknown flash size (%lu/%lu)!\n",
               (unsigned long)mtd->size, (unsigned long)mtd->erasesize);
        return 0;
    }

    #define IOP2_IMAGE_SIZE (245*128*1024)
    bank_size = bank_blks*128*1024;
    high_bank_ofs = (bank_blks + 1) * 128;

    if (rootfs_ofs == 1*128 || rootfs_ofs == high_bank_ofs) {

        /*== Old JFFS2 banked flash layout used ==*/

        update_ofs = (rootfs_ofs == 1*128) ? high_bank_ofs : 1*128;

        /* Boot-sector + nvram */
        bcm63XX_nand_parts[3].offset = 0*1024;
        bcm63XX_nand_parts[3].size = 128*1024;
        bcm63XX_nand_parts[3].ecclayout = mtd->ecclayout;

        /* rootfs */
        bcm63XX_nand_parts[0].offset = rootfs_ofs*1024;
        bcm63XX_nand_parts[0].size = bank_size;
        bcm63XX_nand_parts[0].ecclayout = mtd->ecclayout;

        /* rootfs_update */
        bcm63XX_nand_parts[1].offset = update_ofs*1024;
        bcm63XX_nand_parts[1].size = bank_size;
        bcm63XX_nand_parts[1].ecclayout = mtd->ecclayout;

        /* data (legacy, not used) */
        bcm63XX_nand_parts[2].offset = 984*128*1024;
        bcm63XX_nand_parts[2].size = 32*128*1024;
        bcm63XX_nand_parts[2].ecclayout = mtd->ecclayout;

        /* image (overlaping rootfs) */
        bcm63XX_nand_parts[4].offset = rootfs_ofs*1024;
        bcm63XX_nand_parts[4].size = bank_size;
        bcm63XX_nand_parts[4].ecclayout = mtd->ecclayout;

        /* image_update (overlaping rootfs_update) */
        bcm63XX_nand_parts[5].offset = update_ofs*1024;
        bcm63XX_nand_parts[5].size = bank_size;
        bcm63XX_nand_parts[5].ecclayout = mtd->ecclayout;

        /* rootfs_data (legacy, not used) */
        bcm63XX_nand_parts[6].offset = rootfs_ofs*1024+IOP2_IMAGE_SIZE;
        bcm63XX_nand_parts[6].size = IOP2_IMAGE_SIZE+(1*128*1024);
        bcm63XX_nand_parts[6].ecclayout = mtd->ecclayout;

        /* rootfs_update_data (legacy, not used) */
        bcm63XX_nand_parts[7].offset = update_ofs*1024+IOP2_IMAGE_SIZE;
        bcm63XX_nand_parts[7].size = IOP2_IMAGE_SIZE+(1*128*1024);
        bcm63XX_nand_parts[7].ecclayout = mtd->ecclayout;

        if (kerSysIsRootfsSet() == false) {
            kerSysSetBootParm("root=", "mtd:rootfs");
            kerSysSetBootParm("rootfstype=", "jffs2");
            if (rootfs_ofs == 1*128)
                kerSysSetBootParm("phantomBootParm=", "bank=low");
            else
                kerSysSetBootParm("phantomBootParm=", "bank=high");
        }

    } else {

        if(is_new_fs == 1)
        {
            /*== New UBI volume with large CFE ==*/
            nr_parts = 6;
            /* Boot-sector (old nvram) */
            bcm63XX_nand_parts[3].name = "nvram";
            bcm63XX_nand_parts[3].offset = 0*1024;
            bcm63XX_nand_parts[3].size = 128*1024;
            bcm63XX_nand_parts[3].ecclayout = mtd->ecclayout;
            /* New nvram partition */
            bcm63XX_nand_parts[2].name = "nvram2";
            bcm63XX_nand_parts[2].offset = 1*128*1024;
            bcm63XX_nand_parts[2].size = 3*128*1024;
            bcm63XX_nand_parts[2].ecclayout = mtd->ecclayout;
            /* Kernel 0 partition */
            bcm63XX_nand_parts[0].name = "kernel_0";
            bcm63XX_nand_parts[0].offset = 6*128*1024;
            bcm63XX_nand_parts[0].size = 39*128*1024;
            bcm63XX_nand_parts[0].ecclayout = mtd->ecclayout;
            /* Kernel 1 partition */
            bcm63XX_nand_parts[1].name = "kernel_1";
            bcm63XX_nand_parts[1].offset = 45*128*1024;
            bcm63XX_nand_parts[1].size = 39*128*1024;
            bcm63XX_nand_parts[1].ecclayout = mtd->ecclayout;
            /* Merged partition */
            bcm63XX_nand_parts[4].name = "ubi";
            bcm63XX_nand_parts[4].offset = 84*128*1024;
            bcm63XX_nand_parts[4].size = (num_blks - (84+8))*128*1024;
            bcm63XX_nand_parts[4].ecclayout = mtd->ecclayout;
            bcm63XX_nand_parts[5].name = "cfe_extend";
            bcm63XX_nand_parts[5].offset = 4*128*1024;
            bcm63XX_nand_parts[5].size = 2*128*1024;
            bcm63XX_nand_parts[5].ecclayout = mtd->ecclayout;
        }
        else
        {
            /*== New UBI volume based flash layout used ==*/
            nr_parts = 7;
    
            /* Boot-sector (old nvram) */
            bcm63XX_nand_parts[3].name = "nvram";
            bcm63XX_nand_parts[3].offset = 0*1024;
            bcm63XX_nand_parts[3].size = 128*1024;
            bcm63XX_nand_parts[3].ecclayout = mtd->ecclayout;
    
            /* New nvram partition */
            bcm63XX_nand_parts[2].name = "nvram2";
            bcm63XX_nand_parts[2].offset = 1*128*1024;
            bcm63XX_nand_parts[2].size = 3*128*1024;
            bcm63XX_nand_parts[2].ecclayout = mtd->ecclayout;
    
            /* Kernel 0 partition */
            bcm63XX_nand_parts[0].name = "kernel_0";
            bcm63XX_nand_parts[0].offset = 4*128*1024;
            bcm63XX_nand_parts[0].size = 40*128*1024;
            bcm63XX_nand_parts[0].ecclayout = mtd->ecclayout;
    
            /* Kernel 1 partition */
            bcm63XX_nand_parts[1].name = "kernel_1";
            bcm63XX_nand_parts[1].offset = 44*128*1024;
            bcm63XX_nand_parts[1].size = 40*128*1024;
            bcm63XX_nand_parts[1].ecclayout = mtd->ecclayout;
    
            /* Merged partition */
            bcm63XX_nand_parts[4].name = "ubi";
            bcm63XX_nand_parts[4].offset = 84*128*1024;
            bcm63XX_nand_parts[4].size = (num_blks - (84+8))*128*1024;
            bcm63XX_nand_parts[4].ecclayout = mtd->ecclayout;
    
            /* Old lay-out rootfs 0 partition */
            bcm63XX_nand_parts[5].name = "mtd_lo";
            bcm63XX_nand_parts[5].offset = 84*128*1024;;
            bcm63XX_nand_parts[5].size =
                (high_bank_ofs*1024) - bcm63XX_nand_parts[5].offset;
            bcm63XX_nand_parts[5].ecclayout = mtd->ecclayout;
    
            /* Old lay-out rootfs 1 + data + unused partition */
            bcm63XX_nand_parts[6].name = "mtd_hi";
            bcm63XX_nand_parts[6].offset = high_bank_ofs*1024;
            bcm63XX_nand_parts[6].size =
                (num_blks - 8)*128*1024 - bcm63XX_nand_parts[6].offset;
            bcm63XX_nand_parts[6].ecclayout = mtd->ecclayout;
        }

        /* Last 8 eb's resereved for bad block tables */


    PRINTK("Part[0] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[0].name,
        bcm63XX_nand_parts[0].size, bcm63XX_nand_parts[0].offset);
    PRINTK("Part[1] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[1].name,
        bcm63XX_nand_parts[1].size, bcm63XX_nand_parts[1].offset);
    PRINTK("Part[2] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[2].name,
        bcm63XX_nand_parts[2].size, bcm63XX_nand_parts[2].offset);
    PRINTK("Part[3] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[3].name,
        bcm63XX_nand_parts[3].size, bcm63XX_nand_parts[3].offset);
    PRINTK("Part[4] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[4].name,
        bcm63XX_nand_parts[4].size, bcm63XX_nand_parts[4].offset);
    PRINTK("Part[5] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[5].name,
        bcm63XX_nand_parts[5].size, bcm63XX_nand_parts[5].offset);
    PRINTK("Part[6] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[6].name,
        bcm63XX_nand_parts[6].size, bcm63XX_nand_parts[6].offset);

    mtd_device_register(mtd, bcm63XX_nand_parts, nr_parts);

        if (!kerSysIsRootfsSet()) {
            // Probe which MTD device has UBI
            mtdUbi = get_mtd_device_nm("ubi");
            if(!IS_ERR_OR_NULL(mtdUbi)) {
                snprintf(mtdUbiArg, sizeof(mtdUbiArg), "%d", mtdUbi->index);
                kerSysSetBootParm("ubi.mtd", mtdUbiArg);
                put_mtd_device(mtdUbi);
            }
    
            if ((rootfs_ofs * 1024) == bcm63XX_nand_parts[0].offset)
                kerSysSetBootParm("root=", "ubi:rootfs_0");
            else
                kerSysSetBootParm("root=", "ubi:rootfs_1");
    
            kerSysSetBootParm("rootfstype=", "ubifs");
        }
    }

    return(nr_parts);
#else
    int i = 0;
    uint64_t extra = 0, extra_single_part_size = 0;
    static NVRAM_DATA nvram;
    unsigned int rootfs_ofs;
    int nr_parts = 6;
    int rootfs, rootfs_update, split, image = 4, image_update = 5;
    unsigned long split_offset;


    kerSysNvRamLoad(mtd);
    kerSysBlParmsGetInt(NAND_RFS_OFS_NAME, &rootfs_ofs);
    kerSysNvRamGet((char*)&nvram, sizeof(nvram), 0);

    /* Root FS.  The CFE RAM boot loader saved the rootfs offset that the
     * Linux image was loaded from.
     */
    PRINTK("rootfs_ofs=0x%8.8x, part1ofs=0x%8.8x, part2ofs=0x%8.8x\n",
        rootfs_ofs, nvram.ulNandPartOfsKb[NP_ROOTFS_1],
        nvram.ulNandPartOfsKb[NP_ROOTFS_2]);

    if ( rootfs_ofs == nvram.ulNandPartOfsKb[NP_ROOTFS_1] ) {
        rootfs = NP_ROOTFS_1;
        rootfs_update = NP_ROOTFS_2;
    }else  {
        if ( rootfs_ofs == nvram.ulNandPartOfsKb[NP_ROOTFS_2] ) {
            rootfs = NP_ROOTFS_2;
            rootfs_update = NP_ROOTFS_1;
        }else  {
            /* Backward compatibility with old cferam. */
            extern unsigned char _text;
            unsigned int rootfs_ofs = *(unsigned int*)(&_text - 4);

            if ( rootfs_ofs == nvram.ulNandPartOfsKb[NP_ROOTFS_1] ) {
                rootfs = NP_ROOTFS_1;
                rootfs_update = NP_ROOTFS_2;
            }else  {
                rootfs = NP_ROOTFS_2;
                rootfs_update = NP_ROOTFS_1;
            }
        }
    }

    /* RootFS partition */
    bcm63XX_nand_parts[0].offset = nvram.ulNandPartOfsKb[rootfs] * 1024;
    bcm63XX_nand_parts[0].size = nvram.ulNandPartSizeKb[rootfs] * 1024;
    bcm63XX_nand_parts[0].ecclayout = mtd->ecclayout;


    if (nvram.ulNandPartOfsKb[rootfs] > nvram.ulNandPartOfsKb[rootfs_update]) { // put these in order of partiton so they may be identified later
        image = 5;
        image_update = 4;
    } else { // put these in order of partiton so they may be identified later
        image = 4;
        image_update = 5;
    }

    /* This partition is used for flashing images */
    bcm63XX_nand_parts[image].name = "image";
    bcm63XX_nand_parts[image].offset = bcm63XX_nand_parts[0].offset;
    bcm63XX_nand_parts[image].size = bcm63XX_nand_parts[0].size;
    bcm63XX_nand_parts[image].ecclayout = mtd->ecclayout;

    split = is_split_partition(mtd, bcm63XX_nand_parts[0].offset, bcm63XX_nand_parts[0].size, &split_offset);
    if ((SPLIT_FS == split) || (SQSPLIT_FS == split)) {
        /* RootFS partition */
        bcm63XX_nand_parts[0].offset = split_offset;
        bcm63XX_nand_parts[0].size -= (split_offset - nvram.ulNandPartOfsKb[rootfs] * 1024);

        /* BootFS partition */
        bcm63XX_nand_parts[nr_parts].name = "bootfs";
        bcm63XX_nand_parts[nr_parts].offset = nvram.ulNandPartOfsKb[rootfs] * 1024;
        bcm63XX_nand_parts[nr_parts].size = split_offset - nvram.ulNandPartOfsKb[rootfs] * 1024;
        bcm63XX_nand_parts[nr_parts].ecclayout = mtd->ecclayout;
    }

    printk(">>>>> For primary mtd partition %s, ", bcm63XX_nand_parts[0].name);
    if (split == JFFS2_FS)
        printk("cferam/vmlinux.lz and vmlinux fs mounted as JFFS2 <<<<<\n");
    if (split == SPLIT_FS)
        printk("cferam/vmlinux.lz mounted as JFFS2, vmlinux fs mounted as UBIFS <<<<<\n");
    if (split == UBIFS_FS)
        printk("cferam/vmlinux.lz UBI volume, vmlinux fs mounted as UBIFS <<<<<\n");
    if (split == SQSPLIT_FS)
        printk("cferam/vmlinux.lz mounted as JFFS2, vmlinux fs mounted as squash fs on UBI <<<<<\n");
    if (split == SQUBI_FS)
        printk("cferam/vmlinux.lz UBI volume, vmlinux fs mounted as squash fs on UBI <<<<<\n");

    if ((split == SPLIT_FS) || (split == UBIFS_FS)) {
        if (kerSysIsRootfsSet() == 0) {
            kerSysSetBootParm("ubi.mtd", "0");
            kerSysSetBootParm("root=", "ubi:rootfs_ubifs");
            kerSysSetBootParm("rootfstype=", "ubifs");
        }
    }
    else if ((split == SQSPLIT_FS) || (split == SQUBI_FS)) {
        if (kerSysIsRootfsSet() == 0) {
            kerSysSetBootParm("ubi.mtd", "0");
            kerSysSetBootParm("root=", "/dev/ubiblock0_0");
            kerSysSetBootParm("rootfstype=", "squashfs");
            kerSysSetBootParm("ubi.block", "0,0");
        }
    } else { // JFFS2 filesystem
        if (kerSysIsRootfsSet() == 0) {
            kerSysSetBootParm("root=", "mtd:rootfs");
            kerSysSetBootParm("rootfstype=", "jffs2");
        }
    }

    nr_parts++;

    /* RootFS_update partition */
    bcm63XX_nand_parts[1].offset = nvram.ulNandPartOfsKb[rootfs_update] * 1024;
    bcm63XX_nand_parts[1].size = nvram.ulNandPartSizeKb[rootfs_update] * 1024;
    bcm63XX_nand_parts[1].ecclayout = mtd->ecclayout;

    /* This partition is used for flashing images */
    bcm63XX_nand_parts[image_update].name = "image_update";
    bcm63XX_nand_parts[image_update].offset = bcm63XX_nand_parts[1].offset;
    bcm63XX_nand_parts[image_update].size = bcm63XX_nand_parts[1].size;
    bcm63XX_nand_parts[image_update].ecclayout = mtd->ecclayout;

    split = is_split_partition(mtd, bcm63XX_nand_parts[1].offset, bcm63XX_nand_parts[1].size, &split_offset);
    if ((SPLIT_FS == split) || (SQSPLIT_FS == split)) {
        /* rootfs_update partition */
        bcm63XX_nand_parts[1].offset = split_offset;
        bcm63XX_nand_parts[1].size -= (split_offset - nvram.ulNandPartOfsKb[rootfs_update] * 1024);

        /* bootfs_update partition */
        bcm63XX_nand_parts[nr_parts].name = "bootfs_update";
        bcm63XX_nand_parts[nr_parts].offset = nvram.ulNandPartOfsKb[rootfs_update] * 1024;
        bcm63XX_nand_parts[nr_parts].size = split_offset - nvram.ulNandPartOfsKb[rootfs_update] * 1024;
        bcm63XX_nand_parts[nr_parts].ecclayout = mtd->ecclayout;
        bcm63XX_nand_parts[nr_parts].mask_flags = MTD_WRITEABLE;
    }

    printk("Secondary mtd partition %s detected as ", bcm63XX_nand_parts[1].name);
    if (split == JFFS2_FS)
        printk("JFFS2 for cferam/vmlinux source and vmlinux filesystem\n");
    if (split == SPLIT_FS)
        printk("JFFS2 for cferam/vmlinux source and UBIFS for vmlinux filesystem\n");
    if (split == UBIFS_FS)
        printk("UBI for cferam/vmlinux source and UBIFS for vmlinux filesystem\n");
    if (split == SQSPLIT_FS)
        printk("JFFS2 for cferam/vmlinux source and squash fs on UBI for vmlinux filesystem\n");
    if (split == SQUBI_FS)
        printk("UBI for cferam/vmlinux source and squash fs on UBI for vmlinux filesystem\n");

    nr_parts++;

    /* Data (psi, scratch pad) */
    bcm63XX_nand_parts[2].offset = nvram.ulNandPartOfsKb[NP_DATA] * 1024;
    bcm63XX_nand_parts[2].size = nvram.ulNandPartSizeKb[NP_DATA] * 1024;
    bcm63XX_nand_parts[2].ecclayout = mtd->ecclayout;

    // skip DATA partition
    for (i = BCM_MAX_EXTRA_PARTITIONS - 2; i >= 0; i--) {
        if (nvram.part_info[i].size == 0xffff)
            continue;

        //sz_bits -- 0b01 -- MB, 0b10 - GB , 0b10, 0b11 - reserved
        switch ((nvram.part_info[i].size & 0xc000) >> 14) {
        case 0:
            extra_single_part_size = 1 << 20; //1024*1024;
            break;
        case 1:
            extra_single_part_size = 1 << 30; //1024*1024*1024;
            break;
        default:
            extra_single_part_size = 0;
            break;
        }

        extra_single_part_size = (nvram.part_info[i].size & 0x3fff) * extra_single_part_size;
        if ((extra_single_part_size & (~((uint64_t)mtd->erasesize - 1))) != extra_single_part_size) {
            extra_single_part_size = extra_single_part_size + mtd->erasesize;
        }
        extra_single_part_size = extra_single_part_size & (~((uint64_t)mtd->erasesize - 1));

        if (extra_single_part_size > mtd->erasesize) {
            extra += extra_single_part_size;
            bcm63XX_nand_parts[nr_parts].name = misc_mtd_partition_names[i];
            bcm63XX_nand_parts[nr_parts].offset = (nvram.ulNandPartOfsKb[NP_DATA] * 1024) - extra;
            bcm63XX_nand_parts[nr_parts].size = extra_single_part_size;
            bcm63XX_nand_parts[nr_parts].ecclayout = mtd->ecclayout;
            nr_parts++;
        }
    }

    /* Boot and NVRAM data */
    bcm63XX_nand_parts[3].offset = nvram.ulNandPartOfsKb[NP_BOOT] * 1024;
    bcm63XX_nand_parts[3].size = nvram.ulNandPartSizeKb[NP_BOOT] * 1024;
    bcm63XX_nand_parts[3].ecclayout = mtd->ecclayout;

    PRINTK("Part[0] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[0].name,
           bcm63XX_nand_parts[0].size, bcm63XX_nand_parts[0].offset);
    PRINTK("Part[1] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[1].name,
           bcm63XX_nand_parts[1].size, bcm63XX_nand_parts[1].offset);
    PRINTK("Part[2] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[2].name,
           bcm63XX_nand_parts[2].size, bcm63XX_nand_parts[2].offset);
    PRINTK("Part[3] name=%s, size=%llx, ofs=%llx\n", bcm63XX_nand_parts[3].name,
           bcm63XX_nand_parts[3].size, bcm63XX_nand_parts[3].offset);

    mtd_device_register(mtd, bcm63XX_nand_parts, nr_parts);

    return(nr_parts);
#endif
}
EXPORT_SYMBOL(setup_mtd_parts);
#endif