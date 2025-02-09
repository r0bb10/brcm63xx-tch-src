/*
    i2cmux_i2c.c - I2C client driver for GPON transceiver
    Copyright (C) 2016 Broadcom Corp.

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
#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>  /* kzalloc() */
#include <linux/types.h>
#include <linux/bcm_log.h>
#include <boardparms.h>
#include <i2cmux_i2c.h>

#ifdef PROCFS_HOOKS
#include <asm/uaccess.h> /*copy_from_user*/
#include <linux/proc_fs.h>
#define PROC_DIR_NAME "i2c_mux"
#ifdef GPON_I2C_TEST
#define PROC_ENTRY_NAME "i2cmuxOper"
#endif
#endif


/* I2C client chip addresses */
/* Note that these addresses are 7-bit addresses without the LSB bit
   which indicates R/W operation */
#define I2CMUX_I2C_ADDR1 0x72

/* Addresses to scan */
static unsigned short normal_i2c[] = {I2CMUX_I2C_ADDR1, I2C_CLIENT_END};

/* file system */
enum fs_enum {PROC_FS, SYS_FS};

/* Size of client in bytes */
#define DATA_SIZE             256
#define DWORD_ALIGN           4
#define WORD_ALIGN            2

/* Client0 for Address 0x72 */
#define client0               0

/* Each client has this additional data */
struct i2cmux_data {
    struct i2c_client client;
};

/* Assumption: The i2c modules will be built-in to the kernel and will not be
   unloaded; otherwise, it is possible for caller modules to call the exported
   functions even when the i2c modules are not loaded unless some registration
   mechanism is built-in to this module.  */
static struct i2cmux_data *pclient1_data;

static int i2cmux_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int i2cmux_remove(struct i2c_client *client);
static int i2cmux_detect(struct i2c_client *, struct i2c_board_info *);

/* Check if given offset is valid or not */
static inline int check_offset(u8 offset, u8 alignment)
{
    if (offset % alignment)
    {
        BCM_LOG_ERROR(BCM_LOG_ID_I2C, "Invalid offset %d. The offset should be"
                      " %d byte alligned \n", offset, alignment);
        return -1;
    }
    return 0;
}

ssize_t i2cmux_write(char *buf, size_t count)
{
    struct i2c_client *client = NULL;
    BCM_LOG_DEBUG(BCM_LOG_ID_I2C, "Entering the function %s \n", __FUNCTION__);

    if(count > MAX_TRANSACTION_SIZE)
    {
        BCM_LOG_NOTICE(BCM_LOG_ID_I2C, "count > %d is not yet supported \n", MAX_TRANSACTION_SIZE);
        return -1;
    }

    client = &pclient1_data->client;

    return i2c_master_send(client, buf, count);
}
EXPORT_SYMBOL(i2cmux_write);


ssize_t i2cmux_read(char *buf, size_t count)
{
    struct i2c_client *client = NULL;

    BCM_LOG_DEBUG(BCM_LOG_ID_I2C, "Entering the function %s \n", __FUNCTION__);

    if(count > MAX_TRANSACTION_SIZE)
    {
        BCM_LOG_NOTICE(BCM_LOG_ID_I2C, "count > %d is not yet supported \n", MAX_TRANSACTION_SIZE);
        return -1;
    }

    client = &pclient1_data->client;

    return i2c_master_recv(client, buf, count);
}
EXPORT_SYMBOL(i2cmux_read);



#if defined(SYSFS_HOOKS) || defined(PROCFS_HOOKS)
/* Calls the appropriate function based on user command */
static int exec_command(const char *buf, size_t count, int fs_type)
{
#define MAX_ARGS 1
#define MAX_ARG_SIZE 32
    int i, argc = 0, val = 0;
    char cmd;
    char arg[MAX_ARGS][MAX_ARG_SIZE];
    char temp_buf[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
#ifdef PROCFS_HOOKS
#define LOG_WR_KBUF_SIZE 128
    char kbuf[LOG_WR_KBUF_SIZE];

    if(fs_type == PROC_FS)
    {
        if ((count > LOG_WR_KBUF_SIZE-1) ||
            (copy_from_user(kbuf, buf, count) != 0))
            return -EFAULT;
        kbuf[count]=0;
        argc = sscanf(kbuf, "%c %s", &cmd, arg[0]);
    }
#endif

#ifdef SYSFS_HOOKS
    if(fs_type == SYS_FS)
        argc = sscanf(buf, "%c %s", &cmd, arg[0]);
#endif

    if (argc < 1)
    {
        return -EFAULT;
    }

    for (i=0; i<MAX_ARGS; ++i)
    {
        arg[i][MAX_ARG_SIZE-1] = '\0';
    }

    if (argc == 2)
    {
        val = (int) simple_strtoul(arg[0], NULL, 0);
    }

    switch (cmd)
    {
        case 'w':
            if (argc == 2)
            {
                temp_buf[0] = val;
                printk("0x%x\n",temp_buf[0]);
                i2cmux_write(temp_buf, 1);
                printk("\n");
            }
            break;

        case 'r':
            if (argc == 1)
            {
                i2cmux_read(temp_buf, 1);
                printk("0x%x\n",(u8)temp_buf[0]);
            }
            break;

        default:
            BCM_LOG_NOTICE(BCM_LOG_ID_I2C, "Invalid command. \n Valid commands: \n"

                           "  Write Bytes:     y value count \n"
                           "  Read Bytes:      z count \n");
            break;
    }
    return count;
}
#endif


#ifdef PROCFS_HOOKS
/* Read Function of PROCFS attribute "i2cmuxOper" */
static ssize_t i2cmux_proc_oper_read(struct file *f, char *buf, size_t count,
                               loff_t *pos)
{
    BCM_LOG_NOTICE(BCM_LOG_ID_I2C, " Usage: echo command > "
                   " /proc/i2c-gpon/i2cmuxOper \n");
    BCM_LOG_NOTICE(BCM_LOG_ID_I2C, " supported commands: \n"
                   "  Write Bytes:       w val \n"
                   "  Read Bytes:        r \n"
                   );
    return 0;
}

/* Write Function of PROCFS attribute "i2cmuxOper" */
static ssize_t i2cmux_proc_oper_write(struct file *f, const char *buf,
                                       size_t count, loff_t *pos)
{
    return exec_command(buf, count, PROC_FS);
}
#endif

#ifdef SYSFS_HOOKS
/* Read Function of SYSFS attribute */
static ssize_t i2cmux_sys_read(struct device *dev, struct device_attribute *attr,
                          char *buf)
{
    return snprintf(buf, PAGE_SIZE, "The i2cmux access read attribute \n");
}

/* Write Function of SYSFS attribute */
static ssize_t i2cmux_sys_write(struct device *dev, struct device_attribute *attr,
                           const char *buf, size_t count)
{
    return exec_command(buf, count, SYS_FS);
}

static DEVICE_ATTR(i2cmux_access, S_IRWXUGO, i2cmux_sys_read, i2cmux_sys_write);

static struct attribute *i2cmux_attributes[] = {
    &dev_attr_i2cmux_access.attr,
    NULL
};

static const struct attribute_group i2cmux_attr_group = {
    .attrs = i2cmux_attributes,
};
#endif

#ifdef PROCFS_HOOKS

static struct file_operations i2cmuxOper_fops = {
    .owner  = THIS_MODULE,
    .read = i2cmux_proc_oper_read,
    .write = i2cmux_proc_oper_write
};

static struct proc_dir_entry *q=NULL;
#endif

static const struct i2c_device_id i2cmux_i2c_id_table[] =
{
    { "i2cmux_i2c", 0 },
    { },
};

MODULE_DEVICE_TABLE(i2c, i2cmux_i2c_id_table);

static struct i2c_driver i2cmux_driver = {
    .class = ~0,
    .driver = {
        .name = "i2cmux_i2c",
    },
    .probe  = i2cmux_probe,
    .remove = i2cmux_remove,
    .id_table = i2cmux_i2c_id_table,
    .detect = i2cmux_detect,
    .address_list = normal_i2c
};

static int i2cmux_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int err = 0;
    struct i2cmux_data *pclient_data;
#ifdef PROCFS_HOOKS
    struct proc_dir_entry *p;
#endif

    BCM_LOG_DEBUG(BCM_LOG_ID_I2C, "Entering the function %s \n", __FUNCTION__);

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
        goto exit;

    if (!(pclient_data = kzalloc(sizeof(struct i2cmux_data), GFP_KERNEL)))
    {
        err = -ENOMEM;
        goto exit;
    }

    pclient_data->client.addr = client->addr;
    pclient_data->client.adapter = client->adapter;
    pclient_data->client.flags = client->flags;

    i2c_set_clientdata(client, pclient_data);

    switch(client->addr)
    {
    case I2CMUX_I2C_ADDR1:
        pclient1_data = pclient_data;
        break;
    default:
        BCM_LOG_DEBUG(BCM_LOG_ID_I2C, "%s client addr out of range \n", __FUNCTION__);
        goto exit_kfree;
    }

#ifdef SYSFS_HOOKS
    /* Register sysfs hooks */
    err = sysfs_create_group(&client->dev.kobj, &i2cmux_attr_group);
    if (err)
        goto exit_kfree;
#endif

#ifdef PROCFS_HOOKS
    if (client->addr == I2CMUX_I2C_ADDR1)
    {
        if (!q && (q = proc_mkdir(PROC_DIR_NAME, NULL) ) == NULL) {
            BCM_LOG_ERROR(BCM_LOG_ID_I2C, "i2cmux_i2c: unable to create proc entry\n");
            err = -ENOMEM;
#ifdef SYSFS_HOOKS
            sysfs_remove_group(&client->dev.kobj, &i2cmux_attr_group);
#endif
            goto exit_kfree;
        }
    }

    /* Create only once */
    if (client->addr == I2CMUX_I2C_ADDR1)
        p = proc_create(PROC_ENTRY_NAME, 0644, q, &i2cmuxOper_fops);
#endif

    return 0;

exit_kfree:
    kfree(pclient_data);
exit:
    return err;

}

static int i2cmux_detect(struct i2c_client *client, struct i2c_board_info *info)
{
    BCM_LOG_DEBUG(BCM_LOG_ID_I2C, "Entering the function %s \n", __FUNCTION__);
    strcpy(info->type, "i2cmux_i2c");
    info->flags = 0;
    return 0;
}

static int i2cmux_remove(struct i2c_client *client)
{
    int err = 0;

    BCM_LOG_DEBUG(BCM_LOG_ID_I2C, "Entering the function %s \n", __FUNCTION__);

#ifdef SYSFS_HOOKS
    sysfs_remove_group(&client->dev.kobj, &i2cmux_attr_group);
#endif

#ifdef PROCFS_HOOKS
        remove_proc_entry(PROC_ENTRY_NAME, q);
        remove_proc_entry(PROC_DIR_NAME, NULL);
#endif

    kfree(i2c_get_clientdata(client));

    return err;
}

module_i2c_driver(i2cmux_driver);

MODULE_LICENSE("GPL");

