/*
 * Copyright 2018 NXP
 *
 * PCIe driver for NXP LX SoCs
 * Author: Hou Zhiqiang <Minder.Hou@gmail.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <asm/arch/fsl_serdes.h>
#include <pci.h>
#include <asm/io.h>
#include <errno.h>
#include <malloc.h>
#include <dm.h>
#include "pcie_lx.h"

DECLARE_GLOBAL_DATA_PTR;

LIST_HEAD(lx_pcie_list);

static int lx_pcie_ltssm(struct lx_pcie *pcie)
{
	u32 state;

	state = pf_ctrl_readl(pcie, PCIE_LTSSM_STA) & LTSSM_STATE_MASK;

	return state;
}

static int lx_pcie_link_up(struct lx_pcie *pcie)
{
	int ltssm;

	ltssm = lx_pcie_ltssm(pcie);
	if (ltssm < LTSSM_PCIE_L0)
		return 0;

	return 1;
}

static void lx_pcie_cfg_set_target(struct lx_pcie *pcie, u32 target)
{
	ccsr_writel(pcie, PAB_AXI_AMAP_PEX_WIN_L(0), target);
	ccsr_writel(pcie, PAB_AXI_AMAP_PEX_WIN_H(0), 0);
}

static int lx_pcie_outbound_win_set(struct lx_pcie *pcie, int idx, int type,
				    u64 phys, u64 bus_addr, pci_size_t size)
{
	u32 val;
	u32 size_h, size_l;

	if (idx > PAB_WINS_NUM)
		return -EINVAL;

	size_h = upper_32_bits(~((u64)size - 1));
	size_l = lower_32_bits(~((u64)size - 1));

	ccsr_writel(pcie, PAB_AXI_AMAP_AXI_WIN(idx), lower_32_bits(phys));
	ccsr_writel(pcie, PAB_EXT_AXI_AMAP_AXI_WIN(idx), upper_32_bits(phys));
	ccsr_writel(pcie, PAB_AXI_AMAP_PEX_WIN_L(idx), lower_32_bits(bus_addr));
	ccsr_writel(pcie, PAB_AXI_AMAP_PEX_WIN_H(idx), upper_32_bits(bus_addr));
	ccsr_writel(pcie, PAB_EXT_AXI_AMAP_SIZE(idx), size_h);

	val = ccsr_readl(pcie, PAB_AXI_AMAP_CTRL(idx));
	val &= ~((AXI_AMAP_CTRL_TYPE_MASK << AXI_AMAP_CTRL_TYPE_SHIFT) |
		(AXI_AMAP_CTRL_SIZE_MASK << AXI_AMAP_CTRL_SIZE_SHIFT) |
		AXI_AMAP_CTRL_EN);
	val |= ((type & AXI_AMAP_CTRL_TYPE_MASK) << AXI_AMAP_CTRL_TYPE_SHIFT) |
		((size_l >> AXI_AMAP_CTRL_SIZE_SHIFT) <<
		AXI_AMAP_CTRL_SIZE_SHIFT) | AXI_AMAP_CTRL_EN;

	ccsr_writel(pcie, PAB_AXI_AMAP_CTRL(idx), val);

	return 0;
}

static int lx_pcie_inbound_win_set_rc(struct lx_pcie *pcie, int idx, int type,
				      u64 phys, u64 bus_addr, u64 size)
{
	u32 val;
	u64 win_size = ~(size - 1);

	val = ccsr_readl(pcie, PAB_PEX_AMAP_CTRL(idx));

	val &= ~(PEX_AMAP_CTRL_TYPE_MASK << PEX_AMAP_CTRL_TYPE_SHIFT);
	val &= ~(PEX_AMAP_CTRL_EN_MASK << PEX_AMAP_CTRL_EN_SHIFT);
	val = (val | (type << PEX_AMAP_CTRL_TYPE_SHIFT));
	val = (val | (1 << PEX_AMAP_CTRL_EN_SHIFT));

	ccsr_writel(pcie, PAB_PEX_AMAP_CTRL(idx),
		    val | lower_32_bits(win_size));

	ccsr_writel(pcie, PAB_EXT_PEX_AMAP_SIZE(idx), upper_32_bits(win_size));
	ccsr_writel(pcie, PAB_PEX_AMAP_AXI_WIN(idx), lower_32_bits(phys));
	ccsr_writel(pcie, PAB_EXT_PEX_AMAP_AXI_WIN(idx), upper_32_bits(phys));
	ccsr_writel(pcie, PAB_PEX_AMAP_PEX_WIN_L(idx), lower_32_bits(bus_addr));
	ccsr_writel(pcie, PAB_PEX_AMAP_PEX_WIN_H(idx), upper_32_bits(bus_addr));

	return 0;
}

static void lx_pcie_dump_wins(struct lx_pcie *pcie, int wins)
{
	int i;

	for (i = 0; i < wins; i++) {
		debug("APIO Win%d:\n", i);
		debug("\tLOWER PHYS:	0x%08x\n",
		      ccsr_readl(pcie, PAB_AXI_AMAP_AXI_WIN(i)));
		debug("\tUPPER PHYS:	0x%08x\n",
		      ccsr_readl(pcie, PAB_EXT_AXI_AMAP_AXI_WIN(i)));
		debug("\tLOWER BUS:	0x%08x\n",
		      ccsr_readl(pcie, PAB_AXI_AMAP_PEX_WIN_L(i)));
		debug("\tUPPER BUS:	0x%08x\n",
		      ccsr_readl(pcie, PAB_AXI_AMAP_PEX_WIN_H(i)));
		debug("\tSIZE:		0x%08x\n",
		      ccsr_readl(pcie, PAB_AXI_AMAP_CTRL(i)) &
		      (AXI_AMAP_CTRL_SIZE_MASK << AXI_AMAP_CTRL_SIZE_SHIFT));
		debug("\tEXT_SIZE:	0x%08x\n",
		      ccsr_readl(pcie, PAB_EXT_AXI_AMAP_SIZE(i)));
		debug("\tCTRL:		0x%08x\n",
		      ccsr_readl(pcie, PAB_AXI_AMAP_CTRL(i)));
	}
}

static void lx_pcie_setup_wins(struct lx_pcie *pcie)
{
	struct pci_region *io, *mem, *pref;
	int idx = 1;

	/* INBOUND WIN */
	lx_pcie_inbound_win_set_rc(pcie, 0, IB_TYPE_MEM_NF, 0, 0, SIZE_1T);

	/* OUTBOUND WIN 0: CFG */
	lx_pcie_outbound_win_set(pcie, 0,
				 PAB_AXI_TYPE_CFG,
				 pcie->cfg_res.start,
				 0,
				 fdt_resource_size(&pcie->cfg_res));

	pci_get_regions(pcie->bus, &io, &mem, &pref);

	if (io)
		/* OUTBOUND WIN: IO */
		lx_pcie_outbound_win_set(pcie, idx++,
					 PAB_AXI_TYPE_IO,
					 io->phys_start,
					 io->bus_start,
					 io->size);

	if (mem)
		/* OUTBOUND WIN: MEM */
		lx_pcie_outbound_win_set(pcie, idx++,
					 PAB_AXI_TYPE_MEM,
					 mem->phys_start,
					 mem->bus_start,
					 mem->size);

	if (pref)
		/* OUTBOUND WIN: perf MEM */
		lx_pcie_outbound_win_set(pcie, idx++,
					 PAB_AXI_TYPE_MEM,
					 pref->phys_start,
					 pref->bus_start,
					 pref->size);

	lx_pcie_dump_wins(pcie, idx);
}

/* Return 0 if the address is valid, -errno if not valid */
static int lx_pcie_addr_valid(struct lx_pcie *pcie, pci_dev_t bdf)
{
	struct udevice *bus = pcie->bus;

	if (!pcie->enabled)
		return -ENXIO;

	if (PCI_BUS(bdf) < bus->seq)
		return -EINVAL;

	if ((PCI_BUS(bdf) > bus->seq) && (!lx_pcie_link_up(pcie)))
		return -EINVAL;

	if (PCI_BUS(bdf) <= (bus->seq + 1) && (PCI_DEV(bdf) > 0))
		return -EINVAL;

	return 0;
}

void *lx_pcie_conf_address(struct lx_pcie *pcie, pci_dev_t bdf,
			   int offset)
{
	struct udevice *bus = pcie->bus;
	u32 target;

	if (PCI_BUS(bdf) == bus->seq) {
		if (offset < INDIRECT_ADDR_BNDRY) {
			ccsr_set_page(pcie, 0);
			return pcie->ccsr + offset;
		}

		ccsr_set_page(pcie, OFFSET_TO_PAGE_IDX(offset));
		return pcie->ccsr + OFFSET_TO_PAGE_ADDR(offset);
	}

	target = PAB_TARGET_BUS(PCI_BUS(bdf)) |
		 PAB_TARGET_DEV(PCI_DEV(bdf)) |
		 PAB_TARGET_FUNC(PCI_FUNC(bdf));

	lx_pcie_cfg_set_target(pcie, target);

	return pcie->cfg + offset;
}

static int lx_pcie_read_config(struct udevice *bus, pci_dev_t bdf,
			       uint offset, ulong *valuep,
			       enum pci_size_t size)
{
	struct lx_pcie *pcie = dev_get_priv(bus);
	void *address;
	int ret = 0;

	if (lx_pcie_addr_valid(pcie, bdf)) {
		*valuep = pci_get_ff(size);
		return 0;
	}

	address = lx_pcie_conf_address(pcie, bdf, offset);

	if ((offset == PCI_HEADER_TYPE) | (offset == PCI_VENDOR_ID))
		lut_writel(pcie, 0x0 << PCIE_LUT_GCR_RRE, PCIE_LUT_GCR);

	switch (size) {
	case PCI_SIZE_8:
		*valuep = readb(address);
		break;
	case PCI_SIZE_16:
		*valuep = readw(address);
		break;
	case PCI_SIZE_32:
		*valuep = readl(address);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if ((offset == PCI_HEADER_TYPE) | (offset == PCI_VENDOR_ID))
		lut_writel(pcie, 0x1 << PCIE_LUT_GCR_RRE, PCIE_LUT_GCR);

	return ret;
}

static int lx_pcie_write_config(struct udevice *bus, pci_dev_t bdf,
				uint offset, ulong value,
				enum pci_size_t size)
{
	struct lx_pcie *pcie = dev_get_priv(bus);
	void *address;

	if (lx_pcie_addr_valid(pcie, bdf))
		return 0;

	address = lx_pcie_conf_address(pcie, bdf, offset);

	switch (size) {
	case PCI_SIZE_8:
		writeb(value, address);
		return 0;
	case PCI_SIZE_16:
		writew(value, address);
		return 0;
	case PCI_SIZE_32:
		writel(value, address);
		return 0;
	default:
		return -EINVAL;
	}
}

static void lx_pcie_setup_ctrl(struct lx_pcie *pcie)
{
	u32 val;

	/* Fix class code */
	val = ccsr_readl(pcie, GPEX_CLASSCODE);
	val &= ~(GPEX_CLASSCODE_MASK << GPEX_CLASSCODE_SHIFT);
	val |= PCI_CLASS_BRIDGE_PCI << GPEX_CLASSCODE_SHIFT;
	ccsr_writel(pcie, GPEX_CLASSCODE, val);

	/* Enable APIO and Memory/IO/CFG Wins */
	val = ccsr_readl(pcie, PAB_AXI_PIO_CTRL(0));
	val |= APIO_EN | MEM_WIN_EN | IO_WIN_EN | CFG_WIN_EN;
	ccsr_writel(pcie, PAB_AXI_PIO_CTRL(0), val);

	lx_pcie_setup_wins(pcie);

	pcie->stream_id_cur = 0;
}

static void lx_pcie_ep_inbound_win_set(struct lx_pcie *pcie, int func,
				       int bar, u64 phys)
{
	ccsr_writel(pcie, PAB_EXT_PEX_BAR_AMAP(func, bar), upper_32_bits(phys));
	ccsr_writel(pcie, PAB_PEX_BAR_AMAP(func, bar), lower_32_bits(phys) | 1);
}

static void lx_pcie_ep_setup_wins(struct lx_pcie *pcie)
{
	u64 phys = CONFIG_SYS_PCI_EP_MEMORY_BASE;

	/* WIN 0 : INBOUND : map BAR0 */
	lx_pcie_ep_inbound_win_set(pcie, 1, 0, phys);
	/* WIN 1 : INBOUND : map BAR1 */
	phys += PCIE_BAR1_SIZE;
	lx_pcie_ep_inbound_win_set(pcie, 1, 1, phys);
	/* WIN 2 : INBOUND : map BAR2 */
	phys += PCIE_BAR2_SIZE;
	lx_pcie_ep_inbound_win_set(pcie, 1, 2, phys);
	/* WIN 3 : INBOUND : map BAR4 */
	phys = CONFIG_SYS_PCI_EP_MEMORY_BASE + PCIE_BAR4_SIZE;
	lx_pcie_ep_inbound_win_set(pcie, 1, 3, phys);

	/* WIN 0 : OUTBOUND : map MEM */
	lx_pcie_outbound_win_set(pcie, 0, PAB_AXI_TYPE_MEM, pcie->cfg_res.start,
				 0, CONFIG_SYS_PCI_MEMORY_SIZE);
}
static void lx_pcie_ep_setup_bar(struct lx_pcie *pcie, int bar, u64 size)
{
	u32 val;
	u32 size_l = lower_32_bits(~(size - 1));
	u32 size_h = upper_32_bits(~(size - 1));

	if (size < 4 * 1024)
		return;

	val = ccsr_readl(pcie, PCI_BAR_ENABLE);
	if ((val & 0x0f) != 0x0f)
		ccsr_writel(pcie, PCI_BAR_ENABLE, 0x0f);

	switch (bar) {
	case 0:
	case 1: /* 32bits BAR */
		ccsr_writel(pcie, PCI_BAR_SELECT, bar);
		ccsr_writel(pcie, PCI_BAR_BAR_SIZE_LDW, size_l);
		break;
	case 2:
	case 3: /* 64bits BAR */
		ccsr_writel(pcie, PCI_BAR_SELECT, bar);
		ccsr_writel(pcie, PCI_BAR_BAR_SIZE_LDW, size_l);
		ccsr_writel(pcie, PCI_BAR_BAR_SIZE_UDW, size_h);
		break;
	}
}

static void lx_pcie_ep_setup_bars(struct lx_pcie *pcie)
{
	/* BAR0 - 32bit - 4K configuration */
	lx_pcie_ep_setup_bar(pcie, 0, PCIE_BAR0_SIZE);
	/* BAR1 - 32bit - 8K MSIX */
	lx_pcie_ep_setup_bar(pcie, 1, PCIE_BAR1_SIZE);
	/* BAR2 - 64bit - 4K MEM desciptor */
	lx_pcie_ep_setup_bar(pcie, 2, PCIE_BAR2_SIZE);
	/* BAR4 - 64bit - 1M MEM*/
	lx_pcie_ep_setup_bar(pcie, 3, PCIE_BAR4_SIZE);
}

static void lx_pcie_setup_ep(struct lx_pcie *pcie)
{
	u32 val;

	lx_pcie_ep_setup_bars(pcie);
	lx_pcie_ep_setup_wins(pcie);

	/* Enable APIO and Memory Win */
	val = ccsr_readl(pcie, PAB_AXI_PIO_CTRL(0));
	val |= APIO_EN | MEM_WIN_EN;
	ccsr_writel(pcie, PAB_AXI_PIO_CTRL(0), val);
}

static int lx_pcie_probe(struct udevice *dev)
{
	struct lx_pcie *pcie = dev_get_priv(dev);
	const void *fdt = gd->fdt_blob;
	int node = dev_of_offset(dev);
	u8 header_type;
	u32 link_ctrl_sta;
	bool ep_mode;
	u32 val;
	int ret;

	pcie->bus = dev;

	ret = fdt_get_named_resource(fdt, node, "reg", "reg-names",
				     "ccsr", &pcie->ccsr_res);
	if (ret) {
		printf("lx-pcie: resource \"ccsr\" not found\n");
		return ret;
	}

	pcie->idx = (pcie->ccsr_res.start - PCIE_SYS_BASE_ADDR) /
			PCIE_CCSR_SIZE;

	list_add(&pcie->list, &lx_pcie_list);

	pcie->enabled = is_serdes_configured(PCIE_SRDS_PRTCL(pcie->idx));
	if (!pcie->enabled) {
		printf("PCIe%d: %s disabled\n", pcie->idx, dev->name);
		return 0;
	}

	pcie->ccsr = map_physmem(pcie->ccsr_res.start,
				 fdt_resource_size(&pcie->ccsr_res),
				 MAP_NOCACHE);

	ret = fdt_get_named_resource(fdt, node, "reg", "reg-names",
				     "config", &pcie->cfg_res);
	if (ret) {
		printf("%s: resource \"config\" not found\n", dev->name);
		return ret;
	}

	pcie->cfg = map_physmem(pcie->cfg_res.start,
				fdt_resource_size(&pcie->cfg_res),
				MAP_NOCACHE);

	ret = fdt_get_named_resource(fdt, node, "reg", "reg-names",
				     "lut", &pcie->lut_res);
	if (ret) {
		printf("lx-pcie: resource \"lut\" not found\n");
		return ret;
	}

	pcie->lut = map_physmem(pcie->lut_res.start,
				fdt_resource_size(&pcie->lut_res),
				MAP_NOCACHE);

	ret = fdt_get_named_resource(fdt, node, "reg", "reg-names",
				     "pf_ctrl", &pcie->pf_ctrl_res);
	if (ret) {
		printf("lx-pcie: resource \"pf_ctrl\" not found\n");
		return ret;
	}

	pcie->pf_ctrl = map_physmem(pcie->pf_ctrl_res.start,
				    fdt_resource_size(&pcie->pf_ctrl_res),
				    MAP_NOCACHE);

	pcie->big_endian = fdtdec_get_bool(fdt, node, "big-endian");

	debug("%s ccsr:%lx, cfg:0x%lx, big-endian:%d\n",
	      dev->name, (unsigned long)pcie->ccsr, (unsigned long)pcie->cfg,
	      pcie->big_endian);

	header_type = readb(pcie->ccsr + PCI_HEADER_TYPE);
	ep_mode = (header_type & 0x7f) == PCI_HEADER_TYPE_NORMAL;
	printf("PCIe%u: %s %s", pcie->idx, dev->name,
	       ep_mode ? "Endpoint" : "Root Complex");

	if (ep_mode)
		lx_pcie_setup_ep(pcie);
	else
		lx_pcie_setup_ctrl(pcie);

	val = ccsr_readl(pcie, PAB_PEX_PIO_CTRL(0));
	val |= PPIO_EN;
	ccsr_writel(pcie, PAB_PEX_PIO_CTRL(0), val);

	/* Enable Amba & PEX PIO */
	val = ccsr_readl(pcie, PAB_CTRL);
	val |= PAB_CTRL_APIO_EN | PAB_CTRL_PPIO_EN;
	ccsr_writel(pcie, PAB_CTRL, val);

	if (!lx_pcie_link_up(pcie)) {
		/* Let the user know there's no PCIe link */
		printf(": no link\n");
		return 0;
	}

	/* Print the negotiated PCIe link width */
	link_ctrl_sta = ccsr_readl(pcie, PCIE_LINK_CTRL_STA);
	printf(": x%d gen%d\n",
	       (link_ctrl_sta >> PCIE_LINK_WIDTH_SHIFT & PCIE_LINK_WIDTH_MASK),
	       (link_ctrl_sta >> PCIE_LINK_SPEED_SHIFT) & PCIE_LINK_SPEED_MASK);

	return 0;
}

static const struct dm_pci_ops lx_pcie_ops = {
	.read_config	= lx_pcie_read_config,
	.write_config	= lx_pcie_write_config,
};

static const struct udevice_id lx_pcie_ids[] = {
	{ .compatible = "fsl,lx2160a-pcie" },
	{ }
};

U_BOOT_DRIVER(pcie_lx) = {
	.name = "pcie_lx",
	.id = UCLASS_PCI,
	.of_match = lx_pcie_ids,
	.ops = &lx_pcie_ops,
	.probe	= lx_pcie_probe,
	.priv_auto_alloc_size = sizeof(struct lx_pcie),
};