/*
 * Copyright (C) 2012 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * (EEPROM code originally implemented for rtl8139.c)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * You can also choose to distribute this program under the terms of
 * the Unmodified Binary Distribution Licence (as given in the file
 * COPYING.UBDL), provided that you have satisfied its requirements.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <byteswap.h>
#include <ipxe/netdevice.h>
#include <ipxe/ethernet.h>
#include <ipxe/if_ether.h>
#include <ipxe/iobuf.h>
#include <ipxe/malloc.h>
#include <ipxe/pci.h>
#include <ipxe/dma.h>
#include <ipxe/nvs.h>
#include <ipxe/threewire.h>
#include <ipxe/bitbash.h>
#include <ipxe/mii.h>
#include "realtek.h"

/** @file
 *
 * Realtek 10/100/1000 network card driver
 *
 * Based on the following datasheets:
 *
 *    http://www.datasheetarchive.com/dl/Datasheets-8/DSA-153536.pdf
 *    http://www.datasheetarchive.com/indexdl/Datasheet-028/DSA00494723.pdf
 */

/******************************************************************************
 *
 * Debugging
 *
 ******************************************************************************
 */

/**
 * Dump all registers (for debugging)
 *
 * @v rtl		Realtek device
 */
static __attribute__ (( unused )) void realtek_dump ( struct realtek_nic *rtl ){
	uint8_t regs[256];
	unsigned int i;

	/* Do nothing unless debug output is enabled */
	if ( ! DBG_LOG )
		return;

	/* Dump registers (via byte accesses; may not work for all registers) */
	for ( i = 0 ; i < sizeof ( regs ) ; i++ )
		regs[i] = readb ( rtl->regs + i );
	DBGC ( rtl, "REALTEK %p register dump:\n", rtl );
	DBGC_HDA ( rtl, 0, regs, sizeof ( regs ) );
}

/******************************************************************************
 *
 * EEPROM interface
 *
 ******************************************************************************
 */

/** Pin mapping for SPI bit-bashing interface */
static const uint8_t realtek_eeprom_bits[] = {
	[SPI_BIT_SCLK]	= RTL_9346CR_EESK,
	[SPI_BIT_MOSI]	= RTL_9346CR_EEDI,
	[SPI_BIT_MISO]	= RTL_9346CR_EEDO,
	[SPI_BIT_SS(0)]	= RTL_9346CR_EECS,
};

/**
 * Open bit-bashing interface
 *
 * @v basher		Bit-bashing interface
 */
static void realtek_spi_open_bit ( struct bit_basher *basher ) {
	struct realtek_nic *rtl = container_of ( basher, struct realtek_nic,
						 spibit.basher );

	/* Enable EEPROM access */
	writeb ( RTL_9346CR_EEM_EEPROM, rtl->regs + RTL_9346CR );
	readb ( rtl->regs + RTL_9346CR ); /* Ensure write reaches chip */
}

/**
 * Close bit-bashing interface
 *
 * @v basher		Bit-bashing interface
 */
static void realtek_spi_close_bit ( struct bit_basher *basher ) {
	struct realtek_nic *rtl = container_of ( basher, struct realtek_nic,
						 spibit.basher );

	/* Disable EEPROM access */
	writeb ( RTL_9346CR_EEM_NORMAL, rtl->regs + RTL_9346CR );
	readb ( rtl->regs + RTL_9346CR ); /* Ensure write reaches chip */
}

/**
 * Read input bit
 *
 * @v basher		Bit-bashing interface
 * @v bit_id		Bit number
 * @ret zero		Input is a logic 0
 * @ret non-zero	Input is a logic 1
 */
static int realtek_spi_read_bit ( struct bit_basher *basher,
				  unsigned int bit_id ) {
	struct realtek_nic *rtl = container_of ( basher, struct realtek_nic,
						 spibit.basher );
	uint8_t mask = realtek_eeprom_bits[bit_id];
	uint8_t reg;

	DBG_DISABLE ( DBGLVL_IO );
	reg = readb ( rtl->regs + RTL_9346CR );
	DBG_ENABLE ( DBGLVL_IO );
	return ( reg & mask );
}

/**
 * Set/clear output bit
 *
 * @v basher		Bit-bashing interface
 * @v bit_id		Bit number
 * @v data		Value to write
 */
static void realtek_spi_write_bit ( struct bit_basher *basher,
				    unsigned int bit_id, unsigned long data ) {
	struct realtek_nic *rtl = container_of ( basher, struct realtek_nic,
						 spibit.basher );
	uint8_t mask = realtek_eeprom_bits[bit_id];
	uint8_t reg;

	DBG_DISABLE ( DBGLVL_IO );
	reg = readb ( rtl->regs + RTL_9346CR );
	reg &= ~mask;
	reg |= ( data & mask );
	writeb ( reg, rtl->regs + RTL_9346CR );
	readb ( rtl->regs + RTL_9346CR ); /* Ensure write reaches chip */
	DBG_ENABLE ( DBGLVL_IO );
}

/** SPI bit-bashing interface */
static struct bit_basher_operations realtek_basher_ops = {
	.open = realtek_spi_open_bit,
	.close = realtek_spi_close_bit,
	.read = realtek_spi_read_bit,
	.write = realtek_spi_write_bit,
};

/**
 * Initialise EEPROM
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int realtek_init_eeprom ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	uint16_t id;
	int rc;

	/* Initialise SPI bit-bashing interface */
	rtl->spibit.basher.op = &realtek_basher_ops;
	rtl->spibit.bus.mode = SPI_MODE_THREEWIRE;
	init_spi_bit_basher ( &rtl->spibit );

	/* Detect EEPROM type and initialise three-wire device */
	if ( readl ( rtl->regs + RTL_RCR ) & RTL_RCR_9356SEL ) {
		DBGC ( rtl, "REALTEK %p EEPROM is a 93C56\n", rtl );
		init_at93c56 ( &rtl->eeprom, 16 );
	} else {
		DBGC ( rtl, "REALTEK %p EEPROM is a 93C46\n", rtl );
		init_at93c46 ( &rtl->eeprom, 16 );
	}

	/* Check for EEPROM presence.  Some onboard NICs will have no
	 * EEPROM connected, with the BIOS being responsible for
	 * programming the initial register values.
	 */
	if ( ( rc = nvs_read ( &rtl->eeprom.nvs, RTL_EEPROM_ID,
			       &id, sizeof ( id ) ) ) != 0 ) {
		DBGC ( rtl, "REALTEK %p could not read EEPROM ID: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}
	if ( id != cpu_to_le16 ( RTL_EEPROM_ID_MAGIC ) ) {
		DBGC ( rtl, "REALTEK %p EEPROM ID incorrect (%#04x); assuming "
		       "no EEPROM\n", rtl, le16_to_cpu ( id ) );
		return -ENODEV;
	}

	/* Initialise space for non-volatile options, if available
	 *
	 * We use offset 0x40 (i.e. address 0x20), length 0x40.  This
	 * block is marked as VPD in the Realtek datasheets, so we use
	 * it only if we detect that the card is not supporting VPD.
	 */
	if ( readb ( rtl->regs + RTL_CONFIG1 ) & RTL_CONFIG1_VPD ) {
		DBGC ( rtl, "REALTEK %p EEPROM in use for VPD; cannot use "
		       "for options\n", rtl );
	} else {
		nvo_init ( &rtl->nvo, &rtl->eeprom.nvs, RTL_EEPROM_VPD,
			   RTL_EEPROM_VPD_LEN, NULL, &netdev->refcnt );
	}

	return 0;
}

/******************************************************************************
 *
 * GPHY OCP interface
 *
 ******************************************************************************
 */

/**
 * Read from GPHY OCP register
 *
 * @v rtl		Realtek device
 * @v reg		Register address
 * @ret value		Data read, or negative error
 */
static int realtek_gphy_ocp_read ( struct realtek_nic *rtl, uint32_t reg ) {
	uint32_t value;
	unsigned int i;

	/* OCP registers are 16-bit and must be 2-byte aligned */
	if ( reg & 0xffff0001 )
		return -EINVAL;

	/* Initiate read */
	writel ( ( reg << 15 ), rtl->regs + RTL_GPHY_OCP );

	/* Wait for read to complete */
	for ( i = 0 ; i < RTL_GPHY_OCP_MAX_WAIT_US ; i++ ) {
		value = readl ( rtl->regs + RTL_GPHY_OCP );
		if ( value & RTL_OCPAR_FLAG )
			return ( value & RTL_EPHYAR_DATA_MASK );
		udelay ( 1 );
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for GPHY OCP read\n", rtl );
	return -ETIMEDOUT;
}

/**
 * Write to GPHY OCP register
 *
 * @v rtl		Realtek device
 * @v reg		Register address
 * @v data		Data to write
 */
static void realtek_gphy_ocp_write ( struct realtek_nic *rtl, uint32_t reg,
				     uint32_t data ) {
	unsigned int i;

	/* OCP registers are 16-bit and must be 2-byte aligned */
	if ( reg & 0xffff0001 )
		return;

	/* Initiate write */
	writel ( ( RTL_OCPAR_FLAG | ( reg << 15 ) | data ),
		 rtl->regs + RTL_GPHY_OCP );

	/* Wait for write to complete */
	for ( i = 0 ; i < RTL_GPHY_OCP_MAX_WAIT_US ; i++ ) {
		if ( ! ( readl ( rtl->regs + RTL_GPHY_OCP ) & RTL_OCPAR_FLAG ) )
			return;
		udelay ( 1 );
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for GPHY OCP write\n", rtl );
}

/**
 * Modify GPHY OCP register
 *
 * @v rtl		Realtek device
 * @v reg		Register address
 * @v clear		Bits to clear
 * @v set		Bits to set
 */
static void realtek_gphy_ocp_modify ( struct realtek_nic *rtl, uint32_t reg,
				      uint32_t clear, uint32_t set ) {
	int data;

	data = realtek_gphy_ocp_read ( rtl, reg );
	if ( data < 0 )
		return;
	data &= ~clear;
	data |= set;
	realtek_gphy_ocp_write ( rtl, reg, data );
}

/**
 * Read from CSI register
 *
 * The CSI interface provides access to PCI extended configuration
 * space, which is not reachable via the standard PCI configuration
 * mechanism (as does Linux rtl_csi_read()).
 *
 * @v rtl		Realtek device
 * @v addr		PCI configuration space address
 * @ret value		Data read, or all ones on error
 */
static uint32_t realtek_csi_read ( struct realtek_nic *rtl,
				   unsigned int addr ) {
	uint32_t value;
	unsigned int i;

	/* Initiate read */
	writel ( ( ( addr & RTL_CSIAR_ADDR_MASK ) | RTL_CSIAR_BYTE_ENABLE |
		   ( PCI_FUNC ( rtl->pci->busdevfn ) << 16 ) ),
		 rtl->regs + RTL_CSIAR );

	/* Wait for read to complete */
	for ( i = 0 ; i < RTL_MII_MAX_WAIT_US ; i++ ) {
		value = readl ( rtl->regs + RTL_CSIAR );
		if ( value & RTL_CSIAR_FLAG )
			return readl ( rtl->regs + RTL_CSIDR );
		udelay ( 1 );
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for CSI read\n", rtl );
	return ~0;
}

/**
 * Write to CSI register
 *
 * @v rtl		Realtek device
 * @v addr		PCI configuration space address
 * @v value		Data to write
 */
static void realtek_csi_write ( struct realtek_nic *rtl, unsigned int addr,
				uint32_t value ) {
	unsigned int i;

	/* Initiate write */
	writel ( value, rtl->regs + RTL_CSIDR );
	writel ( ( RTL_CSIAR_FLAG | ( addr & RTL_CSIAR_ADDR_MASK ) |
		   RTL_CSIAR_BYTE_ENABLE |
		   ( PCI_FUNC ( rtl->pci->busdevfn ) << 16 ) ),
		 rtl->regs + RTL_CSIAR );

	/* Wait for write to complete */
	for ( i = 0 ; i < RTL_MII_MAX_WAIT_US ; i++ ) {
		if ( ! ( readl ( rtl->regs + RTL_CSIAR ) & RTL_CSIAR_FLAG ) )
			return;
		udelay ( 1 );
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for CSI write\n", rtl );
}

/**
 * Modify CSI register
 *
 * @v rtl		Realtek device
 * @v addr		PCI configuration space address
 * @v clear		Bits to clear
 * @v set		Bits to set
 */
static void realtek_csi_modify ( struct realtek_nic *rtl, unsigned int addr,
				 uint32_t clear, uint32_t set ) {
	uint32_t value;

	value = realtek_csi_read ( rtl, addr );
	value &= ~clear;
	value |= set;
	realtek_csi_write ( rtl, addr, value );
}

/******************************************************************************
 *
 * MII interface
 *
 ******************************************************************************
 */

/**
 * Read from MII register
 *
 * @v mdio		MII interface
 * @v phy		PHY address
 * @v reg		Register address
 * @ret value		Data read, or negative error
 */
static int realtek_mii_read ( struct mii_interface *mdio,
			      unsigned int phy __unused, unsigned int reg ) {
	struct realtek_nic *rtl =
		container_of ( mdio, struct realtek_nic, mdio );
	unsigned int i;
	uint32_t addr;
	uint32_t value;

	/* Fail if PHYAR register is not present */
	if ( ! rtl->have_phy_regs )
		return -ENOTSUP;

	/* Read via OCP GPHY interface, if applicable */
	if ( rtl->have_ocp ) {
		addr = ( RTL_OCP_STD_PHY_BASE + ( 2 * reg ) );
		return realtek_gphy_ocp_read ( rtl, addr );
	}

	/* Initiate read */
	writel ( RTL_PHYAR_VALUE ( 0, reg, 0 ), rtl->regs + RTL_PHYAR );

	/* Wait for read to complete */
	for ( i = 0 ; i < RTL_MII_MAX_WAIT_US ; i++ ) {

		/* If read is not complete, delay 1us and retry */
		value = readl ( rtl->regs + RTL_PHYAR );
		if ( ! ( value & RTL_PHYAR_FLAG ) ) {
			udelay ( 1 );
			continue;
		}

		/* Return register value */
		return ( RTL_PHYAR_DATA ( value ) );
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for MII read\n", rtl );
	return -ETIMEDOUT;
}

/**
 * Write to MII register
 *
 * @v mdio		MII interface
 * @v phy		PHY address
 * @v reg		Register address
 * @v data		Data to write
 * @ret rc		Return status code
 */
static int realtek_mii_write ( struct mii_interface *mdio,
			       unsigned int phy __unused, unsigned int reg,
			       unsigned int data ) {
	struct realtek_nic *rtl =
		container_of ( mdio, struct realtek_nic, mdio );
	unsigned int i;
	uint32_t addr;

	/* Fail if PHYAR register is not present */
	if ( ! rtl->have_phy_regs )
		return -ENOTSUP;

	/* Write via OCP GPHY interface, if applicable */
	if ( rtl->have_ocp ) {
		addr = ( RTL_OCP_STD_PHY_BASE + ( 2 * reg ) );
		realtek_gphy_ocp_write ( rtl, addr, data );
		return 0;
	}

	/* Initiate write */
	writel ( RTL_PHYAR_VALUE ( RTL_PHYAR_FLAG, reg, data ),
		 rtl->regs + RTL_PHYAR );

	/* Wait for write to complete */
	for ( i = 0 ; i < RTL_MII_MAX_WAIT_US ; i++ ) {

		/* If write is not complete, delay 1us and retry */
		if ( readl ( rtl->regs + RTL_PHYAR ) & RTL_PHYAR_FLAG ) {
			udelay ( 1 );
			continue;
		}

		return 0;
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for MII write\n", rtl );
	return -ETIMEDOUT;
}

/** Realtek MII operations */
static struct mii_operations realtek_mii_operations = {
	.read = realtek_mii_read,
	.write = realtek_mii_write,
};

/******************************************************************************
 *
 * RTL8125 hardware initialisation
 *
 ******************************************************************************
 */

/** An EPHY register initialisation value */
struct realtek_ephy_init {
	/** Register address */
	unsigned int reg;
	/** Mask of bits to preserve */
	unsigned int mask;
	/** Value to write */
	unsigned int value;
};

/** A chip version identifier */
struct realtek_chip_version {
	/** Chip ID mask */
	uint32_t mask;
	/** Chip ID value */
	uint32_t value;
	/** MAC version */
	unsigned int version;
	/** Chip name */
	const char *name;
};

/** RTL8125 chip versions (from Linux r8169 rtl_chip_infos) */
static const struct realtek_chip_version realtek_8125_versions[] = {
	{ 0x7cf, 0x681, 66, "RTL8125BP" },
	{ 0x7cf, 0x708, 65, "RTL8125CP" },
	{ 0x7cf, 0x68b, 64, "RTL9151A" },
	{ 0x7cf, 0x68a, 64, "RTL8125K" },
	{ 0x7cf, 0x689, 64, "RTL8125D" },
	{ 0x7cf, 0x688, 64, "RTL8125D" },
	{ 0x7cf, 0x641, 63, "RTL8125B" },
	{ 0x7cf, 0x609, 61, "RTL8125A" },
};

/** RTL8125A EPHY initialisation values */
static const struct realtek_ephy_init realtek_8125a_ephy[] = {
	{ 0x04, 0xffff, 0xd000 },
	{ 0x0a, 0xffff, 0x8653 },
	{ 0x23, 0xffff, 0xab66 },
	{ 0x20, 0xffff, 0x9455 },
	{ 0x21, 0xffff, 0x99ff },
	{ 0x29, 0xffff, 0xfe04 },
	{ 0x44, 0xffff, 0xd000 },
	{ 0x4a, 0xffff, 0x8653 },
	{ 0x63, 0xffff, 0xab66 },
	{ 0x60, 0xffff, 0x9455 },
	{ 0x61, 0xffff, 0x99ff },
	{ 0x69, 0xffff, 0xfe04 },
};

/** RTL8125B EPHY initialisation values */
static const struct realtek_ephy_init realtek_8125b_ephy[] = {
	{ 0x0b, 0xffff, 0xa908 },
	{ 0x1e, 0xffff, 0x20eb },
	{ 0x4b, 0xffff, 0xa908 },
	{ 0x5e, 0xffff, 0x20eb },
	{ 0x22, 0x0030, 0x0020 },
	{ 0x62, 0x0030, 0x0020 },
};

/**
 * Detect RTL8125 chip version
 *
 * @v rtl		Realtek device
 * @ret rc		Return status code
 */
static int realtek_detect_8125 ( struct realtek_nic *rtl ) {
	const struct realtek_chip_version *chip;
	uint32_t txconfig;
	unsigned int xid;
	unsigned int i;

	/* Read chip ID from TxConfig register */
	txconfig = readl ( rtl->regs + RTL_TCR );
	if ( txconfig == 0xffffffff )
		return -ENODEV;
	xid = ( ( txconfig >> 20 ) & 0xfcf );

	/* Search for matching chip version */
	for ( i = 0 ; i < ( sizeof ( realtek_8125_versions ) /
			    sizeof ( realtek_8125_versions[0] ) ) ; i++ ) {
		chip = &realtek_8125_versions[i];
		if ( ( xid & chip->mask ) == chip->value ) {
			rtl->mac_ver = chip->version;
			DBGC ( rtl, "REALTEK %p is a %s (XID %04x)\n",
			       rtl, chip->name, xid );
			return 0;
		}
	}

	return -ENODEV;
}

/**
 * Read from OCP register
 *
 * @v rtl		Realtek device
 * @v reg		Register address
 * @ret value		Data read
 */
static uint32_t realtek_mac_ocp_read ( struct realtek_nic *rtl,
				       uint32_t reg ) {

	/* OCP registers are 16-bit and must be 2-byte aligned */
	if ( reg & 0xffff0001 )
		return 0;

	/* Initiate read */
	writel ( ( reg << 15 ), rtl->regs + RTL_OCPDR );

	/* Return register value */
	return readl ( rtl->regs + RTL_OCPDR );
}

/**
 * Write to OCP register
 *
 * @v rtl		Realtek device
 * @v reg		Register address
 * @v data		Data to write
 */
static void realtek_mac_ocp_write ( struct realtek_nic *rtl, uint32_t reg,
				    uint32_t data ) {

	/* OCP registers are 16-bit and must be 2-byte aligned */
	if ( reg & 0xffff0001 )
		return;

	/* Initiate write */
	writel ( ( RTL_OCPAR_FLAG | ( reg << 15 ) | data ),
		 rtl->regs + RTL_OCPDR );
}

/**
 * Modify OCP register
 *
 * @v rtl		Realtek device
 * @v reg		Register address
 * @v clear		Bits to clear
 * @v set		Bits to set
 */
static void realtek_mac_ocp_modify ( struct realtek_nic *rtl, uint32_t reg,
				     uint32_t clear, uint32_t set ) {
	uint16_t data;

	data = realtek_mac_ocp_read ( rtl, reg );
	data &= ~clear;
	data |= set;
	realtek_mac_ocp_write ( rtl, reg, data );
}

/**
 * Write to EPHY register
 *
 * @v rtl		Realtek device
 * @v reg		Register address
 * @v value		Value to write
 */
static void realtek_ephy_write ( struct realtek_nic *rtl, unsigned int reg,
				 unsigned int value ) {
	unsigned int i;

	/* Initiate write */
	writel ( ( RTL_EPHYAR_FLAG | ( value & RTL_EPHYAR_DATA_MASK ) |
		   ( ( reg & RTL_EPHYAR_REG_MASK ) << RTL_EPHYAR_REG_SHIFT ) ),
		 rtl->regs + RTL_EPHYAR );

	/* Wait for write to complete */
	for ( i = 0 ; i < RTL_MII_MAX_WAIT_US ; i++ ) {
		if ( ! ( readl ( rtl->regs + RTL_EPHYAR ) & RTL_EPHYAR_FLAG ) )
			return;
		udelay ( 1 );
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for EPHY write\n", rtl );
}

/**
 * Initialise RTL8125 hardware
 *
 * @v rtl		Realtek device
 */
static void realtek_hw_start_8125 ( struct realtek_nic *rtl ) {
	const struct realtek_ephy_init *ephy = NULL;
	unsigned int count = 0;
	unsigned int i;

	/* Configure EPHY registers, as applicable */
	switch ( rtl->mac_ver ) {
	case 61:
		ephy = realtek_8125a_ephy;
		count = ( sizeof ( realtek_8125a_ephy ) /
			  sizeof ( realtek_8125a_ephy[0] ) );
		break;
	case 63:
		ephy = realtek_8125b_ephy;
		count = ( sizeof ( realtek_8125b_ephy ) /
			  sizeof ( realtek_8125b_ephy[0] ) );
		break;
	default:
		break;
	}
	for ( i = 0 ; i < count ; i++ )
		realtek_ephy_write ( rtl, ephy[i].reg, ephy[i].value );

	/* Disable L2/L3 power state (work around an issue when a PCI
	 * reset occurs during L2/L3 state)
	 */
	writeb ( ( readb ( rtl->regs + RTL_CONFIG3 ) & ~RTL_CONFIG3_RDY_TO_L23 ),
		 rtl->regs + RTL_CONFIG3 );

	/* Configure common registers */
	writew ( 0x221b, rtl->regs + 0x382 );
	writel ( 0, rtl->regs + RTL_RSS_CTRL_8125 );
	writew ( 0, rtl->regs + RTL_Q_NUM_CTRL_8125 );

	/* Disable UPS */
	realtek_mac_ocp_modify ( rtl, 0xd40a, 0x0010, 0x0000 );

	/* Disable PCIe clock request */
	writeb ( ( readb ( rtl->regs + RTL_CONFIG1 ) & ~0x10 ),
		 rtl->regs + RTL_CONFIG1 );

	realtek_mac_ocp_write ( rtl, 0xc140, 0xffff );
	realtek_mac_ocp_write ( rtl, 0xc142, 0xffff );
	realtek_mac_ocp_modify ( rtl, 0xd3e2, 0x0fff, 0x03a9 );
	realtek_mac_ocp_modify ( rtl, 0xd3e4, 0x00ff, 0x0000 );
	realtek_mac_ocp_modify ( rtl, 0xe860, 0x0000, 0x0080 );

	/* Disable new TX descriptor format */
	realtek_mac_ocp_modify ( rtl, 0xeb58, 0x0001, 0x0000 );

	/* Disable ZRXDC timeout reporting (as does Linux
	 * rtl_hw_start_8125_common() for the RTL8126A)
	 */
	if ( rtl->mac_ver == 70 )
		writeb ( ( readb ( rtl->regs + 0xd8 ) & ~0x02 ),
			 rtl->regs + 0xd8 );

	/* Configure descriptor ring behaviour */
	if ( rtl->mac_ver == 70 ) {
		realtek_mac_ocp_modify ( rtl, 0xe614, 0x0700, 0x0400 );
	} else if ( rtl->mac_ver == 63 ) {
		realtek_mac_ocp_modify ( rtl, 0xe614, 0x0700, 0x0200 );
		realtek_mac_ocp_modify ( rtl, 0xe63e, 0x0c30, 0x0000 );
	} else {
		realtek_mac_ocp_modify ( rtl, 0xe614, 0x0700, 0x0300 );
		realtek_mac_ocp_modify ( rtl, 0xe63e, 0x0c30, 0x0020 );
	}

	realtek_mac_ocp_modify ( rtl, 0xc0b4, 0x0000, 0x000c );
	realtek_mac_ocp_modify ( rtl, 0xeb6a, 0x00ff, 0x0033 );
	realtek_mac_ocp_modify ( rtl, 0xeb50, 0x03e0, 0x0040 );
	realtek_mac_ocp_modify ( rtl, 0xe056, 0x00f0, 0x0000 );
	realtek_mac_ocp_modify ( rtl, 0xe040, 0x1000, 0x0000 );
	realtek_mac_ocp_modify ( rtl, 0xea1c, 0x0003, 0x0001 );
	if ( rtl->mac_ver == 70 )
		realtek_mac_ocp_modify ( rtl, 0xea1c, 0x0300, 0x0000 );
	else
		realtek_mac_ocp_modify ( rtl, 0xea1c, 0x0004, 0x0000 );
	realtek_mac_ocp_modify ( rtl, 0xe0c0, 0x4f0f, 0x4403 );
	realtek_mac_ocp_modify ( rtl, 0xe052, 0x0080, 0x0068 );
	realtek_mac_ocp_modify ( rtl, 0xd430, 0x0fff, 0x047f );

	realtek_mac_ocp_modify ( rtl, 0xea1c, 0x0004, 0x0000 );
	realtek_mac_ocp_modify ( rtl, 0xeb54, 0x0000, 0x0001 );
	udelay ( 1 );
	realtek_mac_ocp_modify ( rtl, 0xeb54, 0x0001, 0x0000 );
	writew ( ( readw ( rtl->regs + 0x1880 ) & ~0x0030 ),
		 rtl->regs + 0x1880 );

	realtek_mac_ocp_write ( rtl, 0xe098, 0xc302 );

	/* Wait for MAC to complete initialisation */
	for ( i = 0 ; i < 1000 ; i++ ) {
		if ( realtek_mac_ocp_read ( rtl, 0xe00e ) & ( 1 << 13 ) )
			break;
		udelay ( 10 );
	}

	/* Disable RX descriptor gate */
	writel ( ( readl ( rtl->regs + RTL_MISC ) &
		   ~RTL_MISC_RXDV_GATED_EN ), rtl->regs + RTL_MISC );
}

/******************************************************************************
 *
 * RTL8126 hardware initialisation
 *
 ******************************************************************************
 */

/** A GPHY OCP register initialisation value */
struct realtek_phy_ocp_init {
	/** Register address */
	unsigned int reg;
	/** Mask of bits to preserve */
	unsigned int clear;
	/** Value to set */
	unsigned int set;
	/** Write the register (rather than modify in place) */
	unsigned int write;
};

/** RTL8126A GPHY OCP initialisation values (PHY configuration
 * method 1, from the Realtek r8126 driver
 * rtl8126_hw_phy_config_8126a_1())
 */
static const struct realtek_phy_ocp_init realtek_8126a_1_phy[] = {
	{ 0xa442, 0x0000, 0x0800, 0 },
};

/** RTL8126A GPHY OCP initialisation values (PHY configuration
 * method 2, from the Realtek r8126 driver
 * rtl8126_hw_phy_config_8126a_2())
 */
static const struct realtek_phy_ocp_init realtek_8126a_2_phy[] = {
	{ 0xa442, 0x0000, 0x0800, 0 },
	{ 0xa436, 0x0000, 0x80bf, 1 },
	{ 0xa438, 0xff00, 0xed00, 0 },
	{ 0xa436, 0x0000, 0x80cd, 1 },
	{ 0xa438, 0xff00, 0x1000, 0 },
	{ 0xa436, 0x0000, 0x80d1, 1 },
	{ 0xa438, 0xff00, 0xc800, 0 },
	{ 0xa436, 0x0000, 0x80d4, 1 },
	{ 0xa438, 0xff00, 0xc800, 0 },
	{ 0xa436, 0x0000, 0x80e1, 1 },
	{ 0xa438, 0x0000, 0x10cc, 1 },
	{ 0xa436, 0x0000, 0x80e5, 1 },
	{ 0xa438, 0x0000, 0x4f0c, 1 },
	{ 0xa436, 0x0000, 0x8387, 1 },
	{ 0xa438, 0xff00, 0x4700, 0 },
	{ 0xa80c, 0x00c0, 0x0080, 0 },
	{ 0xac90, 0x0010, 0x0000, 0 },
	{ 0xad2c, 0x8000, 0x0000, 0 },
	{ 0xb87c, 0x0000, 0x8321, 1 },
	{ 0xb87e, 0xff00, 0x1100, 0 },
	{ 0xacf8, 0x0000, 0x000c, 0 },
	{ 0xa436, 0x0000, 0x8183, 1 },
	{ 0xa438, 0xff00, 0x5900, 0 },
	{ 0xad94, 0x0000, 0x0020, 0 },
	{ 0xa654, 0x0800, 0x0000, 0 },
	{ 0xb648, 0x0000, 0x4000, 0 },
	{ 0xb87c, 0x0000, 0x839e, 1 },
	{ 0xb87e, 0xff00, 0x2f00, 0 },
	{ 0xb87c, 0x0000, 0x83f2, 1 },
	{ 0xb87e, 0xff00, 0x0800, 0 },
	{ 0xada0, 0x0000, 0x0002, 0 },
	{ 0xb87c, 0x0000, 0x80f3, 1 },
	{ 0xb87e, 0xff00, 0x9900, 0 },
	{ 0xb87c, 0x0000, 0x8126, 1 },
	{ 0xb87e, 0xff00, 0xc100, 0 },
	{ 0xb87c, 0x0000, 0x893a, 1 },
	{ 0xb87e, 0x0000, 0x8080, 1 },
	{ 0xb87c, 0x0000, 0x8647, 1 },
	{ 0xb87e, 0xff00, 0xe600, 0 },
	{ 0xb87c, 0x0000, 0x862c, 1 },
	{ 0xb87e, 0xff00, 0x1200, 0 },
	{ 0xb87c, 0x0000, 0x864a, 1 },
	{ 0xb87e, 0xff00, 0xe600, 0 },
	{ 0xb87c, 0x0000, 0x80a0, 1 },
	{ 0xb87e, 0x0000, 0xbcbc, 1 },
	{ 0xb87c, 0x0000, 0x805e, 1 },
	{ 0xb87e, 0x0000, 0xbcbc, 1 },
	{ 0xb87c, 0x0000, 0x8056, 1 },
	{ 0xb87e, 0x0000, 0x3077, 1 },
	{ 0xb87c, 0x0000, 0x8058, 1 },
	{ 0xb87e, 0xff00, 0x5a00, 0 },
	{ 0xb87c, 0x0000, 0x8098, 1 },
	{ 0xb87e, 0x0000, 0x3077, 1 },
	{ 0xb87c, 0x0000, 0x809a, 1 },
	{ 0xb87e, 0xff00, 0x5a00, 0 },
	{ 0xb87c, 0x0000, 0x8052, 1 },
	{ 0xb87e, 0x0000, 0x3733, 1 },
	{ 0xb87c, 0x0000, 0x8094, 1 },
	{ 0xb87e, 0x0000, 0x3733, 1 },
	{ 0xb87c, 0x0000, 0x807f, 1 },
	{ 0xb87e, 0x0000, 0x7c75, 1 },
	{ 0xb87c, 0x0000, 0x803d, 1 },
	{ 0xb87e, 0x0000, 0x7c75, 1 },
	{ 0xb87c, 0x0000, 0x8036, 1 },
	{ 0xb87e, 0xff00, 0x3000, 0 },
	{ 0xb87c, 0x0000, 0x8078, 1 },
	{ 0xb87e, 0xff00, 0x3000, 0 },
	{ 0xb87c, 0x0000, 0x8031, 1 },
	{ 0xb87e, 0xff00, 0x3300, 0 },
	{ 0xb87c, 0x0000, 0x8073, 1 },
	{ 0xb87e, 0xff00, 0x3300, 0 },
	{ 0xae06, 0xfc00, 0x7c00, 0 },
	{ 0xb87c, 0x0000, 0x89d1, 1 },
	{ 0xb87e, 0x0000, 0x0004, 1 },
	{ 0xa436, 0x0000, 0x8fbd, 1 },
	{ 0xa438, 0xff00, 0x0a00, 0 },
	{ 0xa436, 0x0000, 0x8fbe, 1 },
	{ 0xa438, 0x0000, 0x0d09, 1 },
	{ 0xb87c, 0x0000, 0x89cd, 1 },
	{ 0xb87e, 0x0000, 0x0f0f, 1 },
	{ 0xb87c, 0x0000, 0x89cf, 1 },
	{ 0xb87e, 0x0000, 0x0f0f, 1 },
	{ 0xb87c, 0x0000, 0x83a4, 1 },
	{ 0xb87e, 0x0000, 0x6600, 1 },
	{ 0xb87c, 0x0000, 0x83a6, 1 },
	{ 0xb87e, 0x0000, 0x6601, 1 },
	{ 0xb87c, 0x0000, 0x83c0, 1 },
	{ 0xb87e, 0x0000, 0x6600, 1 },
	{ 0xb87c, 0x0000, 0x83c2, 1 },
	{ 0xb87e, 0x0000, 0x6601, 1 },
	{ 0xb87c, 0x0000, 0x8414, 1 },
	{ 0xb87e, 0x0000, 0x6600, 1 },
	{ 0xb87c, 0x0000, 0x8416, 1 },
	{ 0xb87e, 0x0000, 0x6601, 1 },
	{ 0xb87c, 0x0000, 0x83f8, 1 },
	{ 0xb87e, 0x0000, 0x6600, 1 },
	{ 0xb87c, 0x0000, 0x83fa, 1 },
	{ 0xb87e, 0x0000, 0x6601, 1 },
	{ 0xbd96, 0x1f00, 0x1000, 0 },
	{ 0xbf1c, 0x0007, 0x0007, 0 },
	{ 0xbfbe, 0x8000, 0x0000, 0 },
	{ 0xbf40, 0x0380, 0x0280, 0 },
	{ 0xbf90, 0x0080, 0x0060, 0 },
	{ 0xbf90, 0x0010, 0x000c, 0 },
	{ 0xa436, 0x0000, 0x843b, 1 },
	{ 0xa438, 0xff00, 0x2000, 0 },
	{ 0xa436, 0x0000, 0x843d, 1 },
	{ 0xa438, 0xff00, 0x2000, 0 },
	{ 0xb516, 0x007f, 0x0000, 0 },
	{ 0xbf80, 0x0030, 0x0000, 0 },
	{ 0xa436, 0x0000, 0x8188, 1 },
	{ 0xa438, 0x0000, 0x0044, 1 },
	{ 0xa438, 0x0000, 0x00a8, 1 },
	{ 0xa438, 0x0000, 0x00d6, 1 },
	{ 0xa438, 0x0000, 0x00ec, 1 },
	{ 0xa438, 0x0000, 0x00f6, 1 },
	{ 0xa438, 0x0000, 0x00fc, 1 },
	{ 0xa438, 0x0000, 0x00fe, 1 },
	{ 0xa438, 0x0000, 0x00fe, 1 },
	{ 0xa438, 0x0000, 0x00bc, 1 },
	{ 0xa438, 0x0000, 0x0058, 1 },
	{ 0xa438, 0x0000, 0x002a, 1 },
	{ 0xb87c, 0x0000, 0x8015, 1 },
	{ 0xb87e, 0xff00, 0x0800, 0 },
	{ 0xb87c, 0x0000, 0x8ffd, 1 },
	{ 0xb87e, 0xff00, 0x0000, 0 },
	{ 0xb87c, 0x0000, 0x8fff, 1 },
	{ 0xb87e, 0xff00, 0x7f00, 0 },
	{ 0xb87c, 0x0000, 0x8ffb, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8fe9, 1 },
	{ 0xb87e, 0x0000, 0x0002, 1 },
	{ 0xb87c, 0x0000, 0x8fef, 1 },
	{ 0xb87e, 0x0000, 0x00a5, 1 },
	{ 0xb87c, 0x0000, 0x8ff1, 1 },
	{ 0xb87e, 0x0000, 0x0106, 1 },
	{ 0xb87c, 0x0000, 0x8fe1, 1 },
	{ 0xb87e, 0x0000, 0x0102, 1 },
	{ 0xb87c, 0x0000, 0x8fe3, 1 },
	{ 0xb87e, 0xff00, 0x0400, 0 },
	{ 0xa654, 0x0000, 0x0800, 0 },
	{ 0xa65a, 0x0003, 0x0000, 0 },
	{ 0xac3a, 0x0000, 0x5851, 1 },
	{ 0xac3c, 0xd000, 0x2000, 0 },
	{ 0xac42, 0x0200, 0x01c0, 0 },
	{ 0xac3e, 0xe000, 0x0000, 0 },
	{ 0xac42, 0x0038, 0x0000, 0 },
	{ 0xac42, 0x0002, 0x0005, 0 },
	{ 0xac1a, 0x0000, 0x00db, 1 },
	{ 0xade4, 0x0000, 0x01b5, 1 },
	{ 0xad9c, 0x0c00, 0x0000, 0 },
	{ 0xb87c, 0x0000, 0x814b, 1 },
	{ 0xb87e, 0xff00, 0x1100, 0 },
	{ 0xb87c, 0x0000, 0x814d, 1 },
	{ 0xb87e, 0xff00, 0x1100, 0 },
	{ 0xb87c, 0x0000, 0x814f, 1 },
	{ 0xb87e, 0xff00, 0x0b00, 0 },
	{ 0xb87c, 0x0000, 0x8142, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8144, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8150, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8118, 1 },
	{ 0xb87e, 0xff00, 0x0700, 0 },
	{ 0xb87c, 0x0000, 0x811a, 1 },
	{ 0xb87e, 0xff00, 0x0700, 0 },
	{ 0xb87c, 0x0000, 0x811c, 1 },
	{ 0xb87e, 0xff00, 0x0500, 0 },
	{ 0xb87c, 0x0000, 0x810f, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8111, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x811d, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xac36, 0x0000, 0x1000, 0 },
	{ 0xad1c, 0x0100, 0x0000, 0 },
	{ 0xade8, 0xffc0, 0x1400, 0 },
	{ 0xb87c, 0x0000, 0x864b, 1 },
	{ 0xb87e, 0xff00, 0x9d00, 0 },
	{ 0xa436, 0x0000, 0x8f97, 1 },
	{ 0xa438, 0x0000, 0x003f, 1 },
	{ 0xa438, 0x0000, 0x3f02, 1 },
	{ 0xa438, 0x0000, 0x023c, 1 },
	{ 0xa438, 0x0000, 0x3b0a, 1 },
	{ 0xa438, 0x0000, 0x1c00, 1 },
	{ 0xa438, 0x0000, 0x0000, 1 },
	{ 0xa438, 0x0000, 0x0000, 1 },
	{ 0xa438, 0x0000, 0x0000, 1 },
	{ 0xa438, 0x0000, 0x0000, 1 },
	{ 0xad9c, 0x0000, 0x0020, 0 },
	{ 0xb87c, 0x0000, 0x8122, 1 },
	{ 0xb87e, 0xff00, 0x0c00, 0 },
	{ 0xb87c, 0x0000, 0x82c8, 1 },
	{ 0xb87e, 0x0000, 0x03ed, 1 },
	{ 0xb87e, 0x0000, 0x03ff, 1 },
	{ 0xb87e, 0x0000, 0x0009, 1 },
	{ 0xb87e, 0x0000, 0x03fe, 1 },
	{ 0xb87e, 0x0000, 0x000b, 1 },
	{ 0xb87e, 0x0000, 0x0021, 1 },
	{ 0xb87e, 0x0000, 0x03f7, 1 },
	{ 0xb87e, 0x0000, 0x03b8, 1 },
	{ 0xb87e, 0x0000, 0x03e0, 1 },
	{ 0xb87e, 0x0000, 0x0049, 1 },
	{ 0xb87e, 0x0000, 0x0049, 1 },
	{ 0xb87e, 0x0000, 0x03e0, 1 },
	{ 0xb87e, 0x0000, 0x03b8, 1 },
	{ 0xb87e, 0x0000, 0x03f7, 1 },
	{ 0xb87e, 0x0000, 0x0021, 1 },
	{ 0xb87e, 0x0000, 0x000b, 1 },
	{ 0xb87e, 0x0000, 0x03fe, 1 },
	{ 0xb87e, 0x0000, 0x0009, 1 },
	{ 0xb87e, 0x0000, 0x03ff, 1 },
	{ 0xb87e, 0x0000, 0x03ed, 1 },
	{ 0xb87c, 0x0000, 0x80ef, 1 },
	{ 0xb87e, 0xff00, 0x0c00, 0 },
	{ 0xb87c, 0x0000, 0x82a0, 1 },
	{ 0xb87e, 0x0000, 0x000e, 1 },
	{ 0xb87e, 0x0000, 0x03fe, 1 },
	{ 0xb87e, 0x0000, 0x03ed, 1 },
	{ 0xb87e, 0x0000, 0x0006, 1 },
	{ 0xb87e, 0x0000, 0x001a, 1 },
	{ 0xb87e, 0x0000, 0x03f1, 1 },
	{ 0xb87e, 0x0000, 0x03d8, 1 },
	{ 0xb87e, 0x0000, 0x0023, 1 },
	{ 0xb87e, 0x0000, 0x0054, 1 },
	{ 0xb87e, 0x0000, 0x0322, 1 },
	{ 0xb87e, 0x0000, 0x00dd, 1 },
	{ 0xb87e, 0x0000, 0x03ab, 1 },
	{ 0xb87e, 0x0000, 0x03dc, 1 },
	{ 0xb87e, 0x0000, 0x0027, 1 },
	{ 0xb87e, 0x0000, 0x000e, 1 },
	{ 0xb87e, 0x0000, 0x03e5, 1 },
	{ 0xb87e, 0x0000, 0x03f9, 1 },
	{ 0xb87e, 0x0000, 0x0012, 1 },
	{ 0xb87e, 0x0000, 0x0001, 1 },
	{ 0xb87e, 0x0000, 0x03f1, 1 },
	{ 0xa436, 0x0000, 0x8018, 1 },
	{ 0xa438, 0x0000, 0x2000, 0 },
	{ 0xb87c, 0x0000, 0x8fe4, 1 },
	{ 0xb87e, 0xff00, 0x0000, 0 },
	{ 0xb54c, 0xffc0, 0x3700, 0 },
};

/** RTL8126A GPHY OCP initialisation values (PHY configuration
 * method 3, from the Realtek r8126 driver
 * rtl8126_hw_phy_config_8126a_3())
 */
static const struct realtek_phy_ocp_init realtek_8126a_3_phy[] = {
	{ 0xa442, 0x0000, 0x0800, 0 },
	{ 0xa436, 0x0000, 0x8183, 1 },
	{ 0xa438, 0xff00, 0x5900, 0 },
	{ 0xa654, 0x0000, 0x0800, 0 },
	{ 0xb648, 0x0000, 0x4000, 0 },
	{ 0xad2c, 0x0000, 0x8000, 0 },
	{ 0xad94, 0x0000, 0x0020, 0 },
	{ 0xada0, 0x0000, 0x0002, 0 },
	{ 0xae06, 0xfc00, 0x7c00, 0 },
	{ 0xb87c, 0x0000, 0x8647, 1 },
	{ 0xb87e, 0xff00, 0xe600, 0 },
	{ 0xb87c, 0x0000, 0x8036, 1 },
	{ 0xb87e, 0xff00, 0x3000, 0 },
	{ 0xb87c, 0x0000, 0x8078, 1 },
	{ 0xb87e, 0xff00, 0x3000, 0 },
	{ 0xb87c, 0x0000, 0x89e9, 1 },
	{ 0xb87e, 0x0000, 0xff00, 0 },
	{ 0xb87c, 0x0000, 0x8ffd, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8ffe, 1 },
	{ 0xb87e, 0xff00, 0x0200, 0 },
	{ 0xb87c, 0x0000, 0x8fff, 1 },
	{ 0xb87e, 0xff00, 0x0400, 0 },
	{ 0xa436, 0x0000, 0x8018, 1 },
	{ 0xa438, 0xff00, 0x7700, 0 },
	{ 0xa436, 0x0000, 0x8f9c, 1 },
	{ 0xa438, 0x0000, 0x0005, 1 },
	{ 0xa438, 0x0000, 0x0000, 1 },
	{ 0xa438, 0x0000, 0x00ed, 1 },
	{ 0xa438, 0x0000, 0x0502, 1 },
	{ 0xa438, 0x0000, 0x0b00, 1 },
	{ 0xa438, 0x0000, 0xd401, 1 },
	{ 0xa436, 0x0000, 0x8fa8, 1 },
	{ 0xa438, 0xff00, 0x2900, 0 },
	{ 0xb87c, 0x0000, 0x814b, 1 },
	{ 0xb87e, 0xff00, 0x1100, 0 },
	{ 0xb87c, 0x0000, 0x814d, 1 },
	{ 0xb87e, 0xff00, 0x1100, 0 },
	{ 0xb87c, 0x0000, 0x814f, 1 },
	{ 0xb87e, 0xff00, 0x0b00, 0 },
	{ 0xb87c, 0x0000, 0x8142, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8144, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8150, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8118, 1 },
	{ 0xb87e, 0xff00, 0x0700, 0 },
	{ 0xb87c, 0x0000, 0x811a, 1 },
	{ 0xb87e, 0xff00, 0x0700, 0 },
	{ 0xb87c, 0x0000, 0x811c, 1 },
	{ 0xb87e, 0xff00, 0x0500, 0 },
	{ 0xb87c, 0x0000, 0x810f, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x8111, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xb87c, 0x0000, 0x811d, 1 },
	{ 0xb87e, 0xff00, 0x0100, 0 },
	{ 0xad1c, 0x0000, 0x0100, 0 },
	{ 0xade8, 0xffc0, 0x1400, 0 },
	{ 0xb87c, 0x0000, 0x864b, 1 },
	{ 0xb87e, 0xff00, 0x9d00, 0 },
	{ 0xb87c, 0x0000, 0x862c, 1 },
	{ 0xb87e, 0xff00, 0x1200, 0 },
	{ 0xa436, 0x0000, 0x8566, 1 },
	{ 0xa438, 0x0000, 0x003f, 1 },
	{ 0xa438, 0x0000, 0x3f02, 1 },
	{ 0xa438, 0x0000, 0x023c, 1 },
	{ 0xa438, 0x0000, 0x3b0a, 1 },
	{ 0xa438, 0x0000, 0x1c00, 1 },
	{ 0xa438, 0x0000, 0x0000, 1 },
	{ 0xa438, 0x0000, 0x0000, 1 },
	{ 0xa438, 0x0000, 0x0000, 1 },
	{ 0xa438, 0x0000, 0x0000, 1 },
	{ 0xad9c, 0x0000, 0x0020, 0 },
	{ 0xb87c, 0x0000, 0x8122, 1 },
	{ 0xb87e, 0xff00, 0x0c00, 0 },
	{ 0xb87c, 0x0000, 0x82c8, 1 },
	{ 0xb87e, 0x0000, 0x03ed, 1 },
	{ 0xb87e, 0x0000, 0x03ff, 1 },
	{ 0xb87e, 0x0000, 0x0009, 1 },
	{ 0xb87e, 0x0000, 0x03fe, 1 },
	{ 0xb87e, 0x0000, 0x000b, 1 },
	{ 0xb87e, 0x0000, 0x0021, 1 },
	{ 0xb87e, 0x0000, 0x03f7, 1 },
	{ 0xb87e, 0x0000, 0x03b8, 1 },
	{ 0xb87e, 0x0000, 0x03e0, 1 },
	{ 0xb87e, 0x0000, 0x0049, 1 },
	{ 0xb87e, 0x0000, 0x0049, 1 },
	{ 0xb87e, 0x0000, 0x03e0, 1 },
	{ 0xb87e, 0x0000, 0x03b8, 1 },
	{ 0xb87e, 0x0000, 0x03f7, 1 },
	{ 0xb87e, 0x0000, 0x0021, 1 },
	{ 0xb87e, 0x0000, 0x000b, 1 },
	{ 0xb87e, 0x0000, 0x03fe, 1 },
	{ 0xb87e, 0x0000, 0x0009, 1 },
	{ 0xb87e, 0x0000, 0x03ff, 1 },
	{ 0xb87e, 0x0000, 0x03ed, 1 },
	{ 0xb87c, 0x0000, 0x80ef, 1 },
	{ 0xb87e, 0xff00, 0x0c00, 0 },
	{ 0xb87c, 0x0000, 0x82a0, 1 },
	{ 0xb87e, 0x0000, 0x000e, 1 },
	{ 0xb87e, 0x0000, 0x03fe, 1 },
	{ 0xb87e, 0x0000, 0x03ed, 1 },
	{ 0xb87e, 0x0000, 0x0006, 1 },
	{ 0xb87e, 0x0000, 0x001a, 1 },
	{ 0xb87e, 0x0000, 0x03f1, 1 },
	{ 0xb87e, 0x0000, 0x03d8, 1 },
	{ 0xb87e, 0x0000, 0x0023, 1 },
	{ 0xb87e, 0x0000, 0x0054, 1 },
	{ 0xb87e, 0x0000, 0x0322, 1 },
	{ 0xb87e, 0x0000, 0x00dd, 1 },
	{ 0xb87e, 0x0000, 0x03ab, 1 },
	{ 0xb87e, 0x0000, 0x03dc, 1 },
	{ 0xb87e, 0x0000, 0x0027, 1 },
	{ 0xb87e, 0x0000, 0x000e, 1 },
	{ 0xb87e, 0x0000, 0x03e5, 1 },
	{ 0xb87e, 0x0000, 0x03f9, 1 },
	{ 0xb87e, 0x0000, 0x0012, 1 },
	{ 0xb87e, 0x0000, 0x0001, 1 },
	{ 0xb87e, 0x0000, 0x03f1, 1 },
	{ 0xa430, 0x0000, 0x0003, 0 },
	{ 0xb54c, 0xffc0, 0x3700, 0 },
	{ 0xb648, 0x0000, 0x0040, 0 },
	{ 0xb87c, 0x0000, 0x8082, 1 },
	{ 0xb87e, 0xff00, 0x5d00, 0 },
	{ 0xb87c, 0x0000, 0x807c, 1 },
	{ 0xb87e, 0xff00, 0x5000, 0 },
	{ 0xb87c, 0x0000, 0x809d, 1 },
	{ 0xb87e, 0xff00, 0x5000, 0 },
};

/**
 * Detect RTL8126 chip version
 *
 * @v rtl		Realtek device
 * @ret rc		Return status code
 */
static int realtek_detect_8126 ( struct realtek_nic *rtl ) {
	uint32_t txconfig;
	unsigned int icverid;

	/* Read chip ID from TxConfig register */
	txconfig = readl ( rtl->regs + RTL_TCR );
	if ( txconfig == 0xffffffff )
		return -ENODEV;

	/* Detect RTL8126 (as does the Realtek r8126 driver
	 * rtl8126_get_mac_version())
	 */
	if ( ( txconfig & 0x7c800000 ) != 0x64800000 )
		return -ENODEV;

	/* Determine PHY configuration method from the IC version ID */
	icverid = ( txconfig & 0x00700000 ) >> 20;
	switch ( icverid ) {
	case 0:
		rtl->mcfg = 1;
		break;
	case 1:
		rtl->mcfg = 2;
		break;
	case 2:
		rtl->mcfg = 3;
		break;
	default:
		rtl->mcfg = 3;
		break;
	}

	rtl->mac_ver = 70;
	DBGC ( rtl, "REALTEK %p is an RTL8126A (ICVerID %d, mcfg %d)\n",
	       rtl, icverid, rtl->mcfg );
	return 0;
}

/**
 * Configure RTL8126 PHY
 *
 * @v rtl		Realtek device
 */
static void realtek_hw_phy_config_8126 ( struct realtek_nic *rtl ) {
	const struct realtek_phy_ocp_init *phy = NULL;
	unsigned int count = 0;
	unsigned int i;
	int mcu_version;
	unsigned int mcu_expected;

	/* Check PHY MCU firmware version, as does the Realtek r8126 driver
	 * rtl8126_get_hw_phy_mcu_code_ver().  iPXE has no mechanism to load
	 * the rtl8126a-*.fw firmware files, so the PHY MCU RAM code is not
	 * (re)written here; warn if the hardware version does not match the
	 * version expected by the driver.
	 */
	switch ( rtl->mcfg ) {
	case 1:
		mcu_expected = RTL_MCU_VER_8126A_1;
		break;
	case 2:
		mcu_expected = RTL_MCU_VER_8126A_2;
		break;
	case 3:
		mcu_expected = RTL_MCU_VER_8126A_3;
		break;
	default:
		mcu_expected = 0;
		break;
	}
	if ( mcu_expected != 0 ) {
		realtek_gphy_ocp_write ( rtl, 0xa436, 0x801e );
		mcu_version = realtek_gphy_ocp_read ( rtl, 0xa438 );
		if ( mcu_version < 0 ) {
			DBGC ( rtl, "REALTEK %p could not read PHY MCU firmware "
			       "version\n", rtl );
		} else if ( ( unsigned int ) mcu_version != mcu_expected ) {
			DBGC ( rtl, "REALTEK %p PHY MCU firmware version %#04x "
			       "does not match expected %#04x\n", rtl,
			       mcu_version, mcu_expected );
		}
	}

	/* Configure GPHY OCP registers, as applicable */
	switch ( rtl->mcfg ) {
	case 1:
		phy = realtek_8126a_1_phy;
		count = ( sizeof ( realtek_8126a_1_phy ) /
			  sizeof ( realtek_8126a_1_phy[0] ) );
		break;
	case 2:
		phy = realtek_8126a_2_phy;
		count = ( sizeof ( realtek_8126a_2_phy ) /
			  sizeof ( realtek_8126a_2_phy[0] ) );
		break;
	case 3:
		phy = realtek_8126a_3_phy;
		count = ( sizeof ( realtek_8126a_3_phy ) /
			  sizeof ( realtek_8126a_3_phy[0] ) );
		break;
	default:
		break;
	}
	for ( i = 0 ; i < count ; i++ ) {
		if ( phy[i].write ) {
			/* Write the register directly (as does the Realtek
			 * r8126 driver rtl8126_mdio_direct_write_phy_ocp())
			 */
			realtek_gphy_ocp_write ( rtl, phy[i].reg, phy[i].set );
		} else {
			realtek_gphy_ocp_modify ( rtl, phy[i].reg, phy[i].clear,
						  phy[i].set );
		}
	}

	/* Force legacy power management mode */
	realtek_gphy_ocp_modify ( rtl, 0xa5b4, 0x8000, 0 );

	/* Return to standard MII register page */
	mii_write ( &rtl->mii, 0x1f, 0 );
}

/**
 * Initialise RTL8126 hardware
 *
 * @v rtl		Realtek device
 */
static void realtek_hw_start_8126 ( struct realtek_nic *rtl ) {

	/* Disable ZRXDC timeout reporting (as does Linux
	 * rtl_disable_zrxdc_timeout(); iPXE has no access to PCIe
	 * extended configuration space, so use the CSI fallback)
	 */
	realtek_csi_modify ( rtl, 0x0890, 0x00000001, 0 );

	/* Set default ASPM entry latency (as does Linux
	 * rtl_set_def_aspm_entry_latency(), using the CSI fallback)
	 */
	realtek_csi_modify ( rtl, 0x070c, 0xff000000, 0x27000000 );

	/* Configure common registers */
	realtek_hw_start_8125 ( rtl );

	/* Configure PHY */
	realtek_hw_phy_config_8126 ( rtl );
}

/******************************************************************************
 *
 * Device reset
 *
 ******************************************************************************
 */

/**
 * Reset hardware
 *
 * @v rtl		Realtek device
 * @ret rc		Return status code
 */
static int realtek_reset ( struct realtek_nic *rtl ) {
	unsigned int i;

	/* Issue reset */
	writeb ( RTL_CR_RST, rtl->regs + RTL_CR );

	/* Wait for reset to complete */
	for ( i = 0 ; i < RTL_RESET_MAX_WAIT_MS ; i++ ) {

		/* If reset is not complete, delay 1ms and retry */
		if ( readb ( rtl->regs + RTL_CR ) & RTL_CR_RST ) {
			mdelay ( 1 );
			continue;
		}

		return 0;
	}

	DBGC ( rtl, "REALTEK %p timed out waiting for reset\n", rtl );
	return -ETIMEDOUT;
}

/**
 * Configure PHY for Gigabit operation
 *
 * @v rtl		Realtek device
 * @ret rc		Return status code
 */
static int realtek_phy_speed ( struct realtek_nic *rtl ) {
	int ctrl1000;
	int rc;

	/* Read CTRL1000 register */
	ctrl1000 = mii_read ( &rtl->mii, MII_CTRL1000 );
	if ( ctrl1000 < 0 ) {
		rc = ctrl1000;
		DBGC ( rtl, "REALTEK %p could not read CTRL1000: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}

	/* Advertise 1000Mbps speeds */
	ctrl1000 |= ( ADVERTISE_1000FULL | ADVERTISE_1000HALF );
	if ( ( rc = mii_write ( &rtl->mii, MII_CTRL1000, ctrl1000 ) ) != 0 ) {
		DBGC ( rtl, "REALTEK %p could not write CTRL1000: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Reset PHY
 *
 * @v rtl		Realtek device
 * @ret rc		Return status code
 */
static int realtek_phy_reset ( struct realtek_nic *rtl ) {
	int rc;

	/* Do nothing if we have no separate PHY register access */
	if ( ! rtl->have_phy_regs )
		return 0;

	/* Perform MII reset */
	if ( ( rc = mii_reset ( &rtl->mii ) ) != 0 ) {
		DBGC ( rtl, "REALTEK %p could not reset MII: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}

	/* Some cards (e.g. RTL8169SC) do not advertise Gigabit by
	 * default.  Try to enable advertisement of Gigabit speeds.
	 */
	if ( ( rc = realtek_phy_speed ( rtl ) ) != 0 ) {
		/* Ignore failures, since the register may not be
		 * present on non-Gigabit PHYs (e.g. RTL8101).
		 */
	}

	/* Some cards (e.g. RTL8211B) have a hardware errata that
	 * requires the MII_MMD_DATA register to be cleared before the
	 * link will come up.
	 */
	if ( ( rc = mii_write ( &rtl->mii, MII_MMD_DATA, 0 ) ) != 0 ) {
		/* Ignore failures, since the register may not be
		 * present on all PHYs.
		 */
	}

	/* Restart autonegotiation */
	if ( ( rc = mii_restart ( &rtl->mii ) ) != 0 ) {
		DBGC ( rtl, "REALTEK %p could not restart MII: %s\n",
		       rtl, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/******************************************************************************
 *
 * Link state
 *
 ******************************************************************************
 */

/**
 * Check link state
 *
 * @v netdev		Network device
 */
static void realtek_check_link ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	uint8_t phystatus;
	uint8_t msr;
	int link_up;

	/* Determine link state */
	if ( rtl->have_phy_regs ) {
		mii_dump ( &rtl->mii );
		phystatus = readb ( rtl->regs + RTL_PHYSTATUS );
		link_up = ( phystatus & RTL_PHYSTATUS_LINKSTS );
		DBGC ( rtl, "REALTEK %p PHY status is %02x (%s%s%s%s%s%s, "
		       "Link%s, %sDuplex)\n", rtl, phystatus,
		       ( ( phystatus & RTL_PHYSTATUS_ENTBI ) ? "TBI" : "GMII" ),
		       ( ( phystatus & RTL_PHYSTATUS_TXFLOW ) ?
			 ", TxFlow" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_RXFLOW ) ?
			 ", RxFlow" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_1000MF ) ?
			 ", 1000Mbps" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_100M ) ?
			 ", 100Mbps" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_10M ) ?
			 ", 10Mbps" : "" ),
		       ( ( phystatus & RTL_PHYSTATUS_LINKSTS ) ?
			 "Up" : "Down" ),
		       ( ( phystatus & RTL_PHYSTATUS_FULLDUP ) ?
			 "Full" : "Half" ) );
	} else {
		msr = readb ( rtl->regs + RTL_MSR );
		link_up = ( ! ( msr & RTL_MSR_LINKB ) );
		DBGC ( rtl, "REALTEK %p media status is %02x (Link%s, "
		       "%dMbps%s%s%s%s%s)\n", rtl, msr,
		       ( ( msr & RTL_MSR_LINKB ) ? "Down" : "Up" ),
		       ( ( msr & RTL_MSR_SPEED_10 ) ? 10 : 100 ),
		       ( ( msr & RTL_MSR_TXFCE ) ? ", TxFlow" : "" ),
		       ( ( msr & RTL_MSR_RXFCE ) ? ", RxFlow" : "" ),
		       ( ( msr & RTL_MSR_AUX_STATUS ) ? ", AuxPwr" : "" ),
		       ( ( msr & RTL_MSR_TXPF ) ? ", TxPause" : "" ),
		       ( ( msr & RTL_MSR_RXPF ) ? ", RxPause" : "" ) );
	}

	/* Report link state */
	if ( link_up ) {
		netdev_link_up ( netdev );
	} else {
		netdev_link_down ( netdev );
	}
}

/******************************************************************************
 *
 * Network device interface
 *
 ******************************************************************************
 */

/**
 * Create receive buffer (legacy mode)
 *
 * @v rtl		Realtek device
 * @ret rc		Return status code
 */
static int realtek_create_buffer ( struct realtek_nic *rtl ) {
	struct realtek_rx_buffer *rxbuf = &rtl->rxbuf;
	size_t len = ( RTL_RXBUF_LEN + RTL_RXBUF_PAD );

	/* Do nothing unless in legacy mode */
	if ( ! rtl->legacy )
		return 0;

	/* Allocate buffer */
	rxbuf->data = dma_alloc ( rtl->dma, &rxbuf->map, len,
				  RTL_RXBUF_ALIGN );
	if ( ! rxbuf->data )
		return -ENOMEM;

	/* Program buffer address */
	writel ( dma ( &rxbuf->map, rxbuf->data ), rtl->regs + RTL_RBSTART );
	DBGC ( rtl, "REALTEK %p receive buffer is at [%08lx,%08lx,%08lx)\n",
	       rtl, virt_to_phys ( rxbuf->data ),
	       ( virt_to_phys ( rxbuf->data ) + RTL_RXBUF_LEN ),
	       ( virt_to_phys ( rxbuf->data ) + len ) );

	return 0;
}

/**
 * Destroy receive buffer (legacy mode)
 *
 * @v rtl		Realtek device
 */
static void realtek_destroy_buffer ( struct realtek_nic *rtl ) {
	struct realtek_rx_buffer *rxbuf = &rtl->rxbuf;
	size_t len = ( RTL_RXBUF_LEN + RTL_RXBUF_PAD );

	/* Do nothing unless in legacy mode */
	if ( ! rtl->legacy )
		return;

	/* Clear buffer address */
	writel ( 0, rtl->regs + RTL_RBSTART );

	/* Free buffer */
	dma_free ( &rxbuf->map, rxbuf->data, len );
	rxbuf->data = NULL;
	rxbuf->offset = 0;
}

/**
 * Create descriptor ring
 *
 * @v rtl		Realtek device
 * @v ring		Descriptor ring
 * @ret rc		Return status code
 */
static int realtek_create_ring ( struct realtek_nic *rtl,
				 struct realtek_ring *ring ) {
	physaddr_t address;

	/* Do nothing in legacy mode */
	if ( rtl->legacy )
		return 0;

	/* Allocate descriptor ring */
	ring->desc = dma_alloc ( rtl->dma, &ring->map, ring->len,
				 RTL_RING_ALIGN );
	if ( ! ring->desc )
		return -ENOMEM;

	/* Initialise descriptor ring */
	memset ( ring->desc, 0, ring->len );

	/* Program ring address */
	address = dma ( &ring->map, ring->desc );
	writel ( ( ( ( uint64_t ) address ) >> 32 ),
		 rtl->regs + ring->reg + 4 );
	writel ( ( address & 0xffffffffUL ), rtl->regs + ring->reg );
	DBGC ( rtl, "REALTEK %p ring %02x is at [%08lx,%08lx)\n",
	       rtl, ring->reg, virt_to_phys ( ring->desc ),
	       ( virt_to_phys ( ring->desc ) + ring->len ) );

	return 0;
}

/**
 * Destroy descriptor ring
 *
 * @v rtl		Realtek device
 * @v ring		Descriptor ring
 */
static void realtek_destroy_ring ( struct realtek_nic *rtl,
				   struct realtek_ring *ring ) {

	/* Reset producer and consumer counters */
	ring->prod = 0;
	ring->cons = 0;

	/* Do nothing more if in legacy mode */
	if ( rtl->legacy )
		return;

	/* Clear ring address */
	writel ( 0, rtl->regs + ring->reg );
	writel ( 0, rtl->regs + ring->reg + 4 );

	/* Free descriptor ring */
	dma_free ( &ring->map, ring->desc, ring->len );
	ring->desc = NULL;
}

/**
 * Refill receive descriptor ring
 *
 * @v rtl		Realtek device
 */
static void realtek_refill_rx ( struct realtek_nic *rtl ) {
	struct realtek_descriptor *rx;
	struct io_buffer *iobuf;
	unsigned int rx_idx;
	int is_last;

	/* Do nothing in legacy mode */
	if ( rtl->legacy )
		return;

	while ( ( rtl->rx.prod - rtl->rx.cons ) < RTL_NUM_RX_DESC ) {

		/* Allocate I/O buffer */
		iobuf = alloc_rx_iob ( RTL_RX_MAX_LEN, rtl->dma );
		if ( ! iobuf ) {
			/* Wait for next refill */
			return;
		}

		/* Get next receive descriptor */
		rx_idx = ( rtl->rx.prod++ % RTL_NUM_RX_DESC );
		is_last = ( rx_idx == ( RTL_NUM_RX_DESC - 1 ) );
		rx = &rtl->rx.desc[rx_idx];

		/* Populate receive descriptor */
		rx->address = cpu_to_le64 ( iob_dma ( iobuf ) );
		rx->length = cpu_to_le16 ( RTL_RX_MAX_LEN );
		wmb();
		rx->flags = ( cpu_to_le16 ( RTL_DESC_OWN ) |
			      ( is_last ? cpu_to_le16 ( RTL_DESC_EOR ) : 0 ) );
		wmb();

		/* Record I/O buffer */
		assert ( rtl->rx_iobuf[rx_idx] == NULL );
		rtl->rx_iobuf[rx_idx] = iobuf;

		DBGC2 ( rtl, "REALTEK %p RX %d is [%lx,%lx)\n",
			rtl, rx_idx, virt_to_phys ( iobuf->data ),
			( virt_to_phys ( iobuf->data ) + RTL_RX_MAX_LEN ) );
	}
}

/**
 * Open network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int realtek_open ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	uint32_t tcr;
	uint32_t rcr;
	int rc;

	/* Create transmit descriptor ring */
	if ( ( rc = realtek_create_ring ( rtl, &rtl->tx ) ) != 0 )
		goto err_create_tx;

	/* Create receive descriptor ring */
	if ( ( rc = realtek_create_ring ( rtl, &rtl->rx ) ) != 0 )
		goto err_create_rx;

	/* Create receive buffer */
	if ( ( rc = realtek_create_buffer ( rtl ) ) != 0 )
		goto err_create_buffer;

	/* Initialise RTL8125/RTL8126 hardware, if applicable (as does
	 * Linux rtl_hw_start() on each open)
	 */
	if ( rtl->have_ocp ) {
		if ( rtl->mac_ver == 70 ) {
			realtek_hw_start_8126 ( rtl );
		} else {
			realtek_hw_start_8125 ( rtl );
		}
	}

	/* Accept all packets */
	writel ( 0xffffffffUL, rtl->regs + RTL_MAR0 );
	writel ( 0xffffffffUL, rtl->regs + RTL_MAR4 );

	/* Enable transmitter and receiver.  RTL8139 requires that
	 * this happens before writing to RCR.
	 */
	writeb ( ( RTL_CR_TE | RTL_CR_RE ), rtl->regs + RTL_CR );

	/* Configure transmitter */
	tcr = readl ( rtl->regs + RTL_TCR );
	tcr &= ~RTL_TCR_MXDMA_MASK;
	tcr |= RTL_TCR_MXDMA_DEFAULT;
	writel ( tcr, rtl->regs + RTL_TCR );

	/* Configure receiver */
	rcr = readl ( rtl->regs + RTL_RCR );
	rcr &= ~( RTL_RCR_STOP_WORKING | RTL_RCR_RXFTH_MASK |
		  RTL_RCR_RBLEN_MASK | RTL_RCR_MXDMA_MASK );
	rcr |= ( RTL_RCR_RXFTH_DEFAULT | RTL_RCR_RBLEN_DEFAULT |
		 RTL_RCR_MXDMA_DEFAULT | RTL_RCR_WRAP | RTL_RCR_AB |
		 RTL_RCR_AM | RTL_RCR_APM | RTL_RCR_AAP );

	/* Configure RTL8125-specific receive behaviour (as does Linux
	 * rtl_set_rx_config())
	 */
	if ( rtl->have_ocp )
		rcr |= ( RTL_RCR_FETCH_8125 | RTL_RCR_PAUSE_SLOT_8125 );
	writel ( rcr, rtl->regs + RTL_RCR );

	/* Fill receive ring */
	realtek_refill_rx ( rtl );

	/* Update link state */
	realtek_check_link ( netdev );

	return 0;

	realtek_destroy_buffer ( rtl );
 err_create_buffer:
	realtek_destroy_ring ( rtl, &rtl->rx );
 err_create_rx:
	realtek_destroy_ring ( rtl, &rtl->tx );
 err_create_tx:
	return rc;
}

/**
 * Close network device
 *
 * @v netdev		Network device
 */
static void realtek_close ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	unsigned int i;

	/* Disable receiver and transmitter */
	writeb ( 0, rtl->regs + RTL_CR );

	/* Destroy receive buffer */
	realtek_destroy_buffer ( rtl );

	/* Destroy receive descriptor ring */
	realtek_destroy_ring ( rtl, &rtl->rx );

	/* Discard any unused receive buffers */
	for ( i = 0 ; i < RTL_NUM_RX_DESC ; i++ ) {
		if ( rtl->rx_iobuf[i] )
			free_rx_iob ( rtl->rx_iobuf[i] );
		rtl->rx_iobuf[i] = NULL;
	}

	/* Destroy transmit descriptor ring */
	realtek_destroy_ring ( rtl, &rtl->tx );

	/* Reset legacy transmit descriptor index, if applicable */
	if ( rtl->legacy )
		realtek_reset ( rtl );
}

/**
 * Transmit packet
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 */
static int realtek_transmit ( struct net_device *netdev,
			      struct io_buffer *iobuf ) {
	struct realtek_nic *rtl = netdev->priv;
	struct realtek_descriptor *tx;
	unsigned int tx_idx;
	int is_last;
	int rc;

	/* Get next transmit descriptor */
	if ( ( rtl->tx.prod - rtl->tx.cons ) >= RTL_NUM_TX_DESC ) {
		netdev_tx_defer ( netdev, iobuf );
		return 0;
	}
	tx_idx = ( rtl->tx.prod % RTL_NUM_TX_DESC );

	/* Pad and align packet, if needed */
	if ( rtl->legacy )
		iob_pad ( iobuf, ETH_ZLEN );

	/* Map I/O buffer */
	if ( ( rc = iob_map_tx ( iobuf, rtl->dma ) ) != 0 )
		return rc;

	/* Update producer index */
	rtl->tx.prod++;

	/* Transmit packet */
	if ( rtl->legacy ) {

		/* Add to transmit ring */
		writel ( iob_dma ( iobuf ), rtl->regs + RTL_TSAD ( tx_idx ) );
		writel ( ( RTL_TSD_ERTXTH_DEFAULT | iob_len ( iobuf ) ),
			 rtl->regs + RTL_TSD ( tx_idx ) );

	} else {

		/* Populate transmit descriptor */
		is_last = ( tx_idx == ( RTL_NUM_TX_DESC - 1 ) );
		tx = &rtl->tx.desc[tx_idx];
		tx->address = cpu_to_le64 ( iob_dma ( iobuf ) );
		tx->length = cpu_to_le16 ( iob_len ( iobuf ) );
		wmb();
		tx->flags = ( cpu_to_le16 ( RTL_DESC_OWN | RTL_DESC_FS |
					    RTL_DESC_LS ) |
			      ( is_last ? cpu_to_le16 ( RTL_DESC_EOR ) : 0 ) );
		wmb();

		/* Notify card that there are packets ready to transmit */
		if ( rtl->have_ocp ) {
			writew ( RTL_TPPOLL_8125_NPQ,
				 rtl->regs + RTL_TPPOLL_8125 );
		} else {
			writeb ( RTL_TPPOLL_NPQ, rtl->regs + rtl->tppoll );
		}
	}

	DBGC2 ( rtl, "REALTEK %p TX %d is [%lx,%lx)\n",
		rtl, tx_idx, virt_to_phys ( iobuf->data ),
		virt_to_phys ( iobuf->data ) + iob_len ( iobuf ) );

	return 0;
}

/**
 * Poll for completed packets
 *
 * @v netdev		Network device
 */
static void realtek_poll_tx ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	struct realtek_descriptor *tx;
	unsigned int tx_idx;

	/* Check for completed packets */
	while ( rtl->tx.cons != rtl->tx.prod ) {

		/* Get next transmit descriptor */
		tx_idx = ( rtl->tx.cons % RTL_NUM_TX_DESC );

		/* Stop if descriptor is still in use */
		if ( rtl->legacy ) {

			/* Check ownership bit in transmit status register */
			if ( ! ( readl ( rtl->regs + RTL_TSD ( tx_idx ) ) &
				 RTL_TSD_OWN ) )
				return;

		} else {

			/* Check ownership bit in descriptor */
			tx = &rtl->tx.desc[tx_idx];
			if ( tx->flags & cpu_to_le16 ( RTL_DESC_OWN ) )
				return;
		}

		DBGC2 ( rtl, "REALTEK %p TX %d complete\n", rtl, tx_idx );

		/* Complete TX descriptor */
		rtl->tx.cons++;
		netdev_tx_complete_next ( netdev );
	}
}

/**
 * Poll for received packets (legacy mode)
 *
 * @v netdev		Network device
 */
static void realtek_legacy_poll_rx ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	struct realtek_legacy_header *rx;
	struct io_buffer *iobuf;
	size_t len;

	/* Check for received packets */
	while ( ! ( readb ( rtl->regs + RTL_CR ) & RTL_CR_BUFE ) ) {

		/* Extract packet from receive buffer */
		rx = ( rtl->rxbuf.data + rtl->rxbuf.offset );
		len = le16_to_cpu ( rx->length );
		if ( rx->status & cpu_to_le16 ( RTL_STAT_ROK ) ) {

			DBGC2 ( rtl, "REALTEK %p RX offset %x+%zx\n",
				rtl, rtl->rxbuf.offset, len );

			/* Allocate I/O buffer */
			iobuf = alloc_iob ( len );
			if ( ! iobuf ) {
				netdev_rx_err ( netdev, NULL, -ENOMEM );
				/* Leave packet for next poll */
				break;
			}

			/* Copy data to I/O buffer */
			memcpy ( iob_put ( iobuf, len ), rx->data, len );
			iob_unput ( iobuf, 4 /* strip CRC */ );

			/* Hand off to network stack */
			netdev_rx ( netdev, iobuf );

		} else {

			DBGC ( rtl, "REALTEK %p RX offset %x+%zx error %04x\n",
			       rtl, rtl->rxbuf.offset, len,
			       le16_to_cpu ( rx->status ) );
			netdev_rx_err ( netdev, NULL, -EIO );
		}

		/* Update buffer offset */
		rtl->rxbuf.offset += ( sizeof ( *rx ) + len );
		rtl->rxbuf.offset = ( ( rtl->rxbuf.offset + 3 ) & ~3 );
		rtl->rxbuf.offset = ( rtl->rxbuf.offset % RTL_RXBUF_LEN );
		writew ( ( rtl->rxbuf.offset - 16 ), rtl->regs + RTL_CAPR );

		/* Give chip time to react before rechecking RTL_CR */
		readw ( rtl->regs + RTL_CAPR );
	}
}

/**
 * Poll for received packets
 *
 * @v netdev		Network device
 */
static void realtek_poll_rx ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	struct realtek_descriptor *rx;
	struct io_buffer *iobuf;
	unsigned int rx_idx;
	size_t len;

	/* Poll receive buffer if in legacy mode */
	if ( rtl->legacy ) {
		realtek_legacy_poll_rx ( netdev );
		return;
	}

	/* Check for received packets */
	while ( rtl->rx.cons != rtl->rx.prod ) {

		/* Get next receive descriptor */
		rx_idx = ( rtl->rx.cons % RTL_NUM_RX_DESC );
		rx = &rtl->rx.desc[rx_idx];

		/* Stop if descriptor is still in use */
		if ( rx->flags & cpu_to_le16 ( RTL_DESC_OWN ) )
			return;

		/* Populate I/O buffer */
		iobuf = rtl->rx_iobuf[rx_idx];
		rtl->rx_iobuf[rx_idx] = NULL;
		len = ( le16_to_cpu ( rx->length ) & RTL_DESC_SIZE_MASK );
		iob_put ( iobuf, ( len - 4 /* strip CRC */ ) );

		/* Hand off to network stack */
		if ( rx->flags & cpu_to_le16 ( RTL_DESC_RES ) ) {
			DBGC ( rtl, "REALTEK %p RX %d error (length %zd, "
			       "flags %04x)\n", rtl, rx_idx, len,
			       le16_to_cpu ( rx->flags ) );
			netdev_rx_err ( netdev, iobuf, -EIO );
		} else {
			DBGC2 ( rtl, "REALTEK %p RX %d complete (length "
				"%zd)\n", rtl, rx_idx, len );
			netdev_rx ( netdev, iobuf );
		}
		rtl->rx.cons++;
	}
}

/**
 * Poll for completed and received packets
 *
 * @v netdev		Network device
 */
static void realtek_poll ( struct net_device *netdev ) {
	struct realtek_nic *rtl = netdev->priv;
	uint32_t isr;

	/* Check for and acknowledge interrupts */
	if ( rtl->have_ocp ) {
		isr = readl ( rtl->regs + RTL_ISR_8125 );
		if ( ! isr )
			return;
		writel ( isr, rtl->regs + RTL_ISR_8125 );
	} else {
		isr = readw ( rtl->regs + RTL_ISR );
		if ( ! isr )
			return;
		writew ( isr, rtl->regs + RTL_ISR );
	}

	/* Poll for TX completions, if applicable */
	if ( isr & ( RTL_IRQ_TER | RTL_IRQ_TOK ) )
		realtek_poll_tx ( netdev );

	/* Poll for RX completionsm, if applicable */
	if ( isr & ( RTL_IRQ_RER | RTL_IRQ_ROK ) )
		realtek_poll_rx ( netdev );

	/* Check link state, if applicable */
	if ( isr & RTL_IRQ_PUN_LINKCHG )
		realtek_check_link ( netdev );

	/* Refill RX ring */
	realtek_refill_rx ( rtl );
}

/**
 * Enable or disable interrupts
 *
 * @v netdev		Network device
 * @v enable		Interrupts should be enabled
 */
static void realtek_irq ( struct net_device *netdev, int enable ) {
	struct realtek_nic *rtl = netdev->priv;
	uint32_t imr;

	/* Set interrupt mask */
	imr = ( enable ? ( RTL_IRQ_PUN_LINKCHG | RTL_IRQ_TER | RTL_IRQ_TOK |
			   RTL_IRQ_RER | RTL_IRQ_ROK ) : 0 );
	if ( rtl->have_ocp ) {
		writel ( imr, rtl->regs + RTL_IMR_8125 );
	} else {
		writew ( imr, rtl->regs + RTL_IMR );
	}
}

/** Realtek network device operations */
static struct net_device_operations realtek_operations = {
	.open		= realtek_open,
	.close		= realtek_close,
	.transmit	= realtek_transmit,
	.poll		= realtek_poll,
	.irq		= realtek_irq,
};

/******************************************************************************
 *
 * PCI interface
 *
 ******************************************************************************
 */

/**
 * Detect device type
 *
 * @v rtl		Realtek device
 */
static void realtek_detect ( struct realtek_nic *rtl ) {
	uint16_t rms;
	uint16_t check_rms;
	uint16_t cpcr;
	uint16_t check_cpcr;

	/* Detect RTL8125/RTL8126 (which use OCP register access instead
	 * of PHYAR, and a different TPPoll register)
	 */
	if ( ( realtek_detect_8126 ( rtl ) == 0 ) ||
	     ( realtek_detect_8125 ( rtl ) == 0 ) ) {
		rtl->have_phy_regs = 1;
		rtl->have_ocp = 1;
		rtl->tppoll = RTL_TPPOLL_8125;
		dma_set_mask_64bit ( rtl->dma );
		return;
	}

	/* The RX Packet Maximum Size register is present only on
	 * 8169.  Try to set to our intended MTU.
	 */
	rms = RTL_RX_MAX_LEN;
	writew ( rms, rtl->regs + RTL_RMS );
	check_rms = readw ( rtl->regs + RTL_RMS );

	/* The C+ Command register is present only on 8169 and 8139C+.
	 * Try to enable C+ mode and PCI Dual Address Cycle (for
	 * 64-bit systems), if supported.
	 *
	 * Note that enabling DAC seems to cause bizarre behaviour
	 * (lockups, garbage data on the wire) on some systems, even
	 * if only 32-bit addresses are used.
	 *
	 * Disable VLAN offload, since some cards seem to have it
	 * enabled by default.
	 */
	cpcr = readw ( rtl->regs + RTL_CPCR );
	cpcr |= ( RTL_CPCR_MULRW | RTL_CPCR_CPRX | RTL_CPCR_CPTX );
	if ( sizeof ( physaddr_t ) > sizeof ( uint32_t ) )
		cpcr |= RTL_CPCR_DAC;
	cpcr &= ~RTL_CPCR_VLAN;
	writew ( cpcr, rtl->regs + RTL_CPCR );
	check_cpcr = readw ( rtl->regs + RTL_CPCR );

	/* Detect device type */
	if ( check_rms == rms ) {
		DBGC ( rtl, "REALTEK %p appears to be an RTL8169\n", rtl );
		rtl->have_phy_regs = 1;
		rtl->tppoll = RTL_TPPOLL_8169;
		dma_set_mask_64bit ( rtl->dma );
	} else {
		if ( ( check_cpcr == cpcr ) && ( cpcr != 0xffff ) ) {
			DBGC ( rtl, "REALTEK %p appears to be an RTL8139C+\n",
			       rtl );
			rtl->tppoll = RTL_TPPOLL_8139CP;
			dma_set_mask_64bit ( rtl->dma );
		} else {
			DBGC ( rtl, "REALTEK %p appears to be an RTL8139\n",
			       rtl );
			rtl->legacy = 1;
		}
		rtl->eeprom.bus = &rtl->spibit.bus;
	}
}

/**
 * Probe PCI device
 *
 * @v pci		PCI device
 * @ret rc		Return status code
 */
static int realtek_probe ( struct pci_device *pci ) {
	struct net_device *netdev;
	struct realtek_nic *rtl;
	unsigned int i;
	int rc;

	/* Allocate and initialise net device */
	netdev = alloc_etherdev ( sizeof ( *rtl ) );
	if ( ! netdev ) {
		rc = -ENOMEM;
		goto err_alloc;
	}
	netdev_init ( netdev, &realtek_operations );
	rtl = netdev->priv;
	pci_set_drvdata ( pci, netdev );
	netdev->dev = &pci->dev;
	memset ( rtl, 0, sizeof ( *rtl ) );
	realtek_init_ring ( &rtl->tx, RTL_NUM_TX_DESC, RTL_TNPDS );
	realtek_init_ring ( &rtl->rx, RTL_NUM_RX_DESC, RTL_RDSAR );

	/* Fix up PCI device */
	adjust_pci_device ( pci );

	/* Map registers */
	rtl->regs = pci_ioremap ( pci, pci->membase, RTL_BAR_SIZE );
	if ( ! rtl->regs ) {
		rc = -ENODEV;
		goto err_ioremap;
	}

	/* Configure DMA */
	rtl->dma = &pci->dma;

	/* Record PCI device (for CSI access) */
	rtl->pci = pci;

	/* Reset the NIC */
	if ( ( rc = realtek_reset ( rtl ) ) != 0 )
		goto err_reset;

	/* Detect device type */
	realtek_detect ( rtl );

	/* Initialise RTL8125/RTL8126 hardware, if applicable */
	if ( rtl->have_ocp ) {
		if ( rtl->mac_ver == 70 ) {
			realtek_hw_start_8126 ( rtl );
		} else {
			realtek_hw_start_8125 ( rtl );
		}
	}

	/* Initialise EEPROM */
	if ( rtl->eeprom.bus &&
	     ( ( rc = realtek_init_eeprom ( netdev ) ) == 0 ) ) {

		/* Read MAC address from EEPROM */
		if ( ( rc = nvs_read ( &rtl->eeprom.nvs, RTL_EEPROM_MAC,
				       netdev->hw_addr, ETH_ALEN ) ) != 0 ) {
			DBGC ( rtl, "REALTEK %p could not read MAC address: "
			       "%s\n", rtl, strerror ( rc ) );
			goto err_nvs_read;
		}

	} else {

		/* EEPROM not present.  Fall back to reading the
		 * current ID register value, which will hopefully
		 * have been programmed by the platform firmware.
		 */
		for ( i = 0 ; i < ETH_ALEN ; i++ ) {
			if ( rtl->have_ocp ) {
				netdev->hw_addr[i] =
					readb ( rtl->regs + RTL_MAC0_BKP + i );
			} else {
				netdev->hw_addr[i] =
					readb ( rtl->regs + RTL_IDR0 + i );
			}
		}
	}

	/* Initialise and reset MII interface */
	mdio_init ( &rtl->mdio, &realtek_mii_operations );
	mii_init ( &rtl->mii, &rtl->mdio, 0 );
	if ( ( rc = realtek_phy_reset ( rtl ) ) != 0 )
		goto err_phy_reset;

	/* Register network device */
	if ( ( rc = register_netdev ( netdev ) ) != 0 )
		goto err_register_netdev;

	/* Set initial link state */
	realtek_check_link ( netdev );

	/* Register non-volatile options, if applicable */
	if ( rtl->nvo.nvs ) {
		if ( ( rc = register_nvo ( &rtl->nvo,
					   netdev_settings ( netdev ) ) ) != 0)
			goto err_register_nvo;
	}

	return 0;

 err_register_nvo:
	unregister_netdev ( netdev );
 err_register_netdev:
 err_phy_reset:
 err_nvs_read:
	realtek_reset ( rtl );
 err_reset:
	iounmap ( rtl->regs );
 err_ioremap:
	netdev_nullify ( netdev );
	netdev_put ( netdev );
 err_alloc:
	return rc;
}

/**
 * Remove PCI device
 *
 * @v pci		PCI device
 */
static void realtek_remove ( struct pci_device *pci ) {
	struct net_device *netdev = pci_get_drvdata ( pci );
	struct realtek_nic *rtl = netdev->priv;

	/* Unregister non-volatile options, if applicable */
	if ( rtl->nvo.nvs )
		unregister_nvo ( &rtl->nvo );

	/* Unregister network device */
	unregister_netdev ( netdev );

	/* Reset card */
	realtek_reset ( rtl );

	/* Free network device */
	iounmap ( rtl->regs );
	netdev_nullify ( netdev );
	netdev_put ( netdev );
}

/** Realtek PCI device IDs */
static struct pci_device_id realtek_nics[] = {
	PCI_ROM ( 0x0001, 0x8168, "clone8169",	"Cloned 8169", 0 ),
	PCI_ROM ( 0x018a, 0x0106, "fpc0106tx",	"LevelOne FPC-0106TX", 0 ),
	PCI_ROM ( 0x021b, 0x8139, "hne300",	"Compaq HNE-300", 0 ),
	PCI_ROM ( 0x02ac, 0x1012, "s1012",	"SpeedStream 1012", 0 ),
	PCI_ROM ( 0x0357, 0x000a, "ttpmon",	"TTTech TTP-Monitoring", 0 ),
	PCI_ROM ( 0x10ec, 0x8126, "rtl8126",	"RTL8126 5Gbps", 0 ),
	PCI_ROM ( 0x10ec, 0x8125, "rtl8125",	"RTL8125 2.5Gbps", 0 ),
	PCI_ROM ( 0x10ec, 0x8129, "rtl8129",	"RTL-8129", 0 ),
	PCI_ROM ( 0x10ec, 0x8136, "rtl8136",	"RTL8101E/RTL8102E", 0 ),
	PCI_ROM ( 0x10ec, 0x8138, "rtl8138",	"RT8139 (B/C)", 0 ),
	PCI_ROM ( 0x10ec, 0x8139, "rtl8139",	"RTL-8139/8139C/8139C+", 0 ),
	PCI_ROM ( 0x10ec, 0x8167, "rtl8167",	"RTL-8110SC/8169SC", 0 ),
	PCI_ROM ( 0x10ec, 0x8168, "rtl8168",	"RTL8111/8168B", 0 ),
	PCI_ROM ( 0x10ec, 0x8169, "rtl8169",	"RTL-8169", 0 ),
	PCI_ROM ( 0x1113, 0x1211, "smc1211",	"SMC2-1211TX", 0 ),
	PCI_ROM ( 0x1186, 0x1300, "dfe538",	"DFE530TX+/DFE538TX", 0 ),
	PCI_ROM ( 0x1186, 0x1340, "dfe690",	"DFE-690TXD", 0 ),
	PCI_ROM ( 0x1186, 0x4300, "dge528t",	"DGE-528T", 0 ),
	PCI_ROM ( 0x11db, 0x1234, "sega8139",	"Sega Enterprises 8139", 0 ),
	PCI_ROM ( 0x1259, 0xa117, "allied8139",	"Allied Telesyn 8139", 0 ),
	PCI_ROM ( 0x1259, 0xa11e, "allied81xx",	"Allied Telesyn 81xx", 0 ),
	PCI_ROM ( 0x1259, 0xc107, "allied8169",	"Allied Telesyn 8169", 0 ),
	PCI_ROM ( 0x126c, 0x1211, "northen8139","Northern Telecom 8139", 0 ),
	PCI_ROM ( 0x13d1, 0xab06, "fe2000vx",	"Abocom FE2000VX", 0 ),
	PCI_ROM ( 0x1432, 0x9130, "edi8139",	"Edimax 8139", 0 ),
	PCI_ROM ( 0x14ea, 0xab06, "fnw3603tx",	"Planex FNW-3603-TX", 0 ),
	PCI_ROM ( 0x14ea, 0xab07, "fnw3800tx",	"Planex FNW-3800-TX", 0 ),
	PCI_ROM ( 0x1500, 0x1360, "delta8139",	"Delta Electronics 8139", 0 ),
	PCI_ROM ( 0x16ec, 0x0116, "usr997902",	"USR997902", 0 ),
	PCI_ROM ( 0x1737, 0x1032, "linksys8169","Linksys 8169", 0 ),
	PCI_ROM ( 0x1743, 0x8139, "rolf100",	"Peppercorn ROL/F-100", 0 ),
	PCI_ROM ( 0x4033, 0x1360, "addron8139",	"Addtron 8139", 0 ),
	PCI_ROM ( 0xffff, 0x8139, "clonse8139",	"Cloned 8139", 0 ),
};

/** Realtek PCI driver */
struct pci_driver realtek_driver __pci_driver = {
	.ids = realtek_nics,
	.id_count = ( sizeof ( realtek_nics ) / sizeof ( realtek_nics[0] ) ),
	.probe = realtek_probe,
	.remove = realtek_remove,
};
