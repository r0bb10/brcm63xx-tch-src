/*
 * RNG driver for Broadcom RNGs
 *
 * Copyright 2009 (c) Broadcom Inc.
 *
 * with the majority of the code coming from:
 *
 * Hardware driver for the Intel/AMD/VIA Random Number Generators (RNG)
 * (c) Copyright 2003 Red Hat Inc <jgarzik@redhat.com>
 *
 * derived from
 *
 * Hardware driver for the AMD 768 Random Number Generator (RNG)
 * (c) Copyright 2001 Red Hat Inc <alan@redhat.com>
 *
 * derived from
 *
 * Hardware driver for Intel i810 Random Number Generator (RNG)
 * Copyright 2000,2001 Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2000,2001 Philipp Rumpf <prumpf@mandrakesoft.com>
 *
 * This file is licensed under  the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/hw_random.h>
#include <asm/io.h>
#include <asm/delay.h>

#if defined(CONFIG_BCM963268) || defined(CONFIG_BCM96362)
#include <bcm_map_part.h>
#define SPU_CLOCK       (*(volatile unsigned long *)(PERF_BASE+0x04))
#define RNG_CTRL        (*(volatile unsigned long *)(IPSEC_BASE+0x80))
#define RNG_STAT        (*(volatile unsigned long *)(IPSEC_BASE+0x84))
#define RNG_DATA        (*(volatile unsigned long *)(IPSEC_BASE+0x88))
#else
#define IPSEC_CLK_EN                 (1 << 18)
#define SPU_CLOCK                    (*(volatile unsigned long *)0xb0000004)
#define SPU_CTRL                     (*(volatile unsigned long *)0xb0004100)
#define RNG_CTRL                     (*(volatile unsigned long *)0xb0004180)
#define RNG_STAT                     (*(volatile unsigned long *)0xb0004184)
#define RNG_DATA                     (*(volatile unsigned long *)0xb0004188)
#define RNG_THRES                    (*(volatile unsigned long *)0xb000418c)
#define RNG_MASK                     (*(volatile unsigned long *)0xb0004190)
#endif

#define PFX	KBUILD_MODNAME ": "

static int bcm_rng_data_present(struct hwrng *rng)
{
      	return (RNG_STAT & 0xFF000000);
}

static int bcm_rng_data_read(struct hwrng *rng, u32 *data)
{
	*data = RNG_DATA;

	return 4;
}

static int bcm_rng_init(struct hwrng *rng)
{
	unsigned long rng_enable;
	int do_reset = 0;

	/* Enable IPSec SPU clock */
#ifdef MISC_IDDQ_CTRL_IPSEC
	if ( MISC->miscIddqCtrl & MISC_IDDQ_CTRL_IPSEC) do_reset = 1;
	MISC->miscIddqCtrl &= ~MISC_IDDQ_CTRL_IPSEC;
#endif

#ifdef IPSEC_CLK_EN
	if ( PERF->blkEnables & IPSEC_CLK_EN) do_reset = 1;
	PERF->blkEnables |= IPSEC_CLK_EN;
#endif

	/* if ipsec block was not already started do a reset, must be done once */
#ifdef SOFT_RST_IPSEC
	if (do_reset){
		PERF->softResetB &= ~SOFT_RST_IPSEC;
		udelay(100);
		PERF->softResetB |= SOFT_RST_IPSEC;
	}
#endif

	/* Enable TRNG random number generation */
	rng_enable = RNG_CTRL;
        rng_enable = rng_enable | 0x00000001;
        RNG_CTRL = rng_enable;

	return 0;
}

static void bcm_rng_cleanup(struct hwrng *rng)
{
	return 0;
}


static struct hwrng bcm_rng = {
	.name		= "bcm",
	.init		= bcm_rng_init,
	.cleanup	= bcm_rng_cleanup,
	.data_present	= bcm_rng_data_present,
	.data_read	= bcm_rng_data_read,
};


static int __init mod_init(void)
{
	int err = -ENODEV;


	bcm_rng.priv = 0xb0004184;
	err = hwrng_register(&bcm_rng);
	if (err) {
		printk(KERN_ERR PFX "RNG registering failed (%d)\n",
		       err);
	}
	return err;
}

static void __exit mod_exit(void)
{
	hwrng_unregister(&bcm_rng);
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_AUTHOR("The Linux Kernel team");
MODULE_DESCRIPTION("H/W RNG driver for BCM chipsets");
MODULE_LICENSE("GPL");
