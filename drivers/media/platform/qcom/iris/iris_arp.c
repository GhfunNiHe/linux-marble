// SPDX-License-Identifier: GPL-2.0-only
/*
 * Encoder ARP buffers in the CP non-pixel region (VPU2)
 */

#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <dt-bindings/firmware/qcom,scm.h>

#include "iris_buffer.h"
#include "iris_core.h"
#include "iris_instance.h"

static int iris_arp_assign(struct iris_core *core, bool to_secure)
{
	struct iris_arp_pool *arp = &core->arp;
	struct qcom_scm_vmperm perm;
	u64 src;

	if (to_secure) {
		src = BIT(QCOM_SCM_VMID_HLOS);
		perm.vmid = QCOM_SCM_VMID_CP_NON_PIXEL;
		perm.perm = QCOM_SCM_PERM_RW;
	} else {
		src = BIT(QCOM_SCM_VMID_CP_NON_PIXEL);
		perm.vmid = QCOM_SCM_VMID_HLOS;
		perm.perm = QCOM_SCM_PERM_RWX;
	}

	return qcom_scm_assign_mem(arp->base, arp->size, &src, &perm, 1);
}

int iris_arp_pool_init(struct iris_core *core)
{
	const struct iris_platform_data *pdata = core->iris_platform_data;
	struct iris_arp_pool *arp = &core->arp;
	struct platform_device_info info = {};
	struct platform_device *pdev;
	struct iommu_domain *domain;
	struct device_node *np;
	struct resource res;
	void *vaddr;
	int ret;

	np = of_get_child_by_name(core->dev->of_node, "non-pixel");
	if (!np)
		return 0;

	if (!pdata->tz_cp_config_data_size) {
		ret = -EINVAL;
		goto err_put_node;
	}

	ret = of_reserved_mem_region_to_resource(np, 0, &res);
	if (ret) {
		dev_err(core->dev, "non-pixel: no memory-region: %d\n", ret);
		goto err_put_node;
	}

	arp->base = res.start;
	arp->size = round_down(resource_size(&res), IRIS_ARP_SLOT_SIZE);
	arp->size = min_t(size_t, arp->size, BITS_PER_LONG * IRIS_ARP_SLOT_SIZE);
	arp->iova_top = pdata->tz_cp_config_data[0].cp_nonpixel_start +
			pdata->tz_cp_config_data[0].cp_nonpixel_size;
	if (!arp->size || !IS_ALIGNED(arp->base, SZ_4K) ||
	    arp->iova_top < arp->size) {
		ret = -EINVAL;
		goto err_put_node;
	}

	/* Clear the carveout while HLOS still owns it */
	vaddr = memremap(arp->base, arp->size, MEMREMAP_WC);
	if (!vaddr) {
		ret = -ENOMEM;
		goto err_put_node;
	}
	memset(vaddr, 0, arp->size);
	memunmap(vaddr);

	info.fwnode = of_fwnode_handle(np);
	info.parent = core->dev;
	info.name = np->name;
	info.dma_mask = DMA_BIT_MASK(32);

	pdev = platform_device_register_full(&info);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto err_put_node;
	}
	pdev->dev.of_node = np;

	ret = of_dma_configure(&pdev->dev, np, true);
	if (ret) {
		dev_err(core->dev, "non-pixel: dma configure failed: %d\n", ret);
		goto err_unregister;
	}

	domain = iommu_paging_domain_alloc(&pdev->dev);
	if (IS_ERR(domain)) {
		ret = PTR_ERR(domain);
		goto err_unregister;
	}

	ret = iommu_attach_device(domain, &pdev->dev);
	if (ret) {
		dev_err(core->dev, "non-pixel: attach failed: %d\n", ret);
		goto err_domain_free;
	}

	arp->dev = &pdev->dev;
	arp->domain = domain;

	ret = iris_arp_assign(core, true);
	if (ret) {
		dev_err(core->dev, "non-pixel: assign to CP_NON_PIXEL failed: %d\n", ret);
		goto err_detach;
	}

	ret = iommu_map(domain, arp->iova_top - arp->size, arp->base, arp->size,
			IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret) {
		dev_err(core->dev, "non-pixel: map failed: %d\n", ret);
		goto err_reclaim;
	}

	dev_info(core->dev, "ARP pool: %zu KiB at iova %#x-%#x (CP non-pixel)\n",
		 arp->size / SZ_1K, arp->iova_top - (u32)arp->size, arp->iova_top);

	of_node_put(np);

	return 0;

err_reclaim:
	if (iris_arp_assign(core, false)) {
		/* Still owned by CP_NON_PIXEL: never hand it back */
		dev_err(core->dev, "non-pixel: reclaim failed, leaking carveout\n");
	}
err_detach:
	iommu_detach_device(domain, &pdev->dev);
err_domain_free:
	iommu_domain_free(domain);
err_unregister:
	platform_device_unregister(pdev);
err_put_node:
	of_node_put(np);
	memset(arp, 0, sizeof(*arp));
	arp->required = true;

	return ret;
}

void iris_arp_pool_deinit(struct iris_core *core)
{
	struct iris_arp_pool *arp = &core->arp;

	if (!arp->dev)
		return;

	iommu_unmap(arp->domain, arp->iova_top - arp->size, arp->size);
	if (iris_arp_assign(core, false))
		dev_err(core->dev, "non-pixel: reclaim failed, leaking carveout\n");

	iommu_detach_device(arp->domain, arp->dev);
	iommu_domain_free(arp->domain);
	platform_device_unregister(to_platform_device(arp->dev));
	memset(arp, 0, sizeof(*arp));
}

bool iris_arp_pool_enabled(struct iris_core *core)
{
	return core->arp.dev;
}

int iris_arp_alloc(struct iris_inst *inst, struct iris_buffer *buffer)
{
	struct iris_core *core = inst->core;
	struct iris_arp_pool *arp = &core->arp;
	unsigned int nslots = arp->size / IRIS_ARP_SLOT_SIZE;
	unsigned int slot;

	if (buffer->buffer_size > IRIS_ARP_SLOT_SIZE)
		return -EINVAL;

	do {
		slot = find_first_zero_bit(&arp->used, nslots);
	} while (slot < nslots && test_and_set_bit(slot, &arp->used));

	if (slot >= nslots) {
		dev_err(core->dev, "no free ARP slot (%u in use)\n", nslots);
		return -EBUSY;
	}

	/* Slot 0 sits at the top of the range, like the stock driver's ARP */
	buffer->device_addr = arp->iova_top - (slot + 1) * IRIS_ARP_SLOT_SIZE;
	buffer->kvaddr = NULL;
	buffer->dma_attrs = 0;

	return 0;
}

void iris_arp_free(struct iris_inst *inst, struct iris_buffer *buffer)
{
	struct iris_arp_pool *arp = &inst->core->arp;
	unsigned int slot;

	slot = (arp->iova_top - buffer->device_addr) / IRIS_ARP_SLOT_SIZE - 1;

	clear_bit(slot, &arp->used);
}
