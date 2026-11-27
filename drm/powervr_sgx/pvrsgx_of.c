// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * An SGX on a device tree SoC (Documentation/devicetree/bindings/gpu/
 * img,powervr-sgx.yaml). On TI parts the GPU node has no clocks or power
 * domain of its own: its ti-sysc parent module enables both, and applies the
 * SGX interrupt quirk, when the GPU device is runtime-resumed. Every register
 * access happens with the parent held awake.
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/sizes.h>

#include "pvrsgx_device.h"
#include "pvrsgx_drv.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_pm.h"

/*
 * Bring-up (SGX530, default off): start the kernel data heap here instead of
 * the layout's, inside its range, to see whether an address the 3D reads
 * moves with it.
 */
static unsigned int probe_kdata_base;
module_param(probe_kdata_base, uint, 0444);
MODULE_PARM_DESC(probe_kdata_base, "Bring-up (SGX530): kernel data heap base, 64 KiB aligned (default 0: the layout's)");

static void probe_heaps(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_va_range *kd = &sdev->family->heaps->kernel_data;

	if (!probe_kdata_base || sdev->family->core != 530)
		return;
	if (probe_kdata_base & (SZ_64K - 1) || probe_kdata_base < kd->base ||
	    probe_kdata_base + SZ_1M > kd->end) {
		dev_warn(sdev->base.dev, "probe: kernel data base 0x%08x refused\n",
			 probe_kdata_base);
		return;
	}
	sdev->heaps_probe = *sdev->family->heaps;
	sdev->heaps_probe.kernel_data.base = probe_kdata_base;
	sdev->heaps = &sdev->heaps_probe;
	dev_warn(sdev->base.dev, "probe: kernel data heap at 0x%08x\n", probe_kdata_base);
}

static int pvrsgx_of_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pvrsgx_device *sdev;
	struct resource *res, win;
	int err, irq;

	/* Clocks and resets on the GPU node itself (non-TI parts) aren't handled yet. */
	if (of_property_present(dev->of_node, "clocks") ||
	    of_property_present(dev->of_node, "resets")) {
		dev_err(dev, "GPU node with its own clocks or resets; not touching it\n");
		return -ENODEV;
	}
	if (!dev->parent || !pm_runtime_enabled(dev->parent)) {
		dev_err(dev, "no runtime-PM parent to clock the GPU; not touching it\n");
		return -ENODEV;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < PVRSGX_REGS_MIN)
		return -ENODEV;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err)
		return err;
	dma_set_max_seg_size(dev, UINT_MAX);

	sdev = pvrsgx_alloc(dev);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);
	sdev->family = pvrsgx_core_family_get((uintptr_t)device_get_match_data(dev));
	if (sdev->family)
		probe_heaps(sdev);
	sdev->irq = irq;
	platform_set_drvdata(pdev, sdev);

	/* Map only the registers: nothing past them is named (OMAP3 maps 32 MiB). */
	win = DEFINE_RES_MEM_NAMED(res->start, PVRSGX_REGS_MIN, PVRSGX_DRIVER_NAME);
	sdev->regs = devm_ioremap_resource(dev, &win);
	if (IS_ERR(sdev->regs))
		return PTR_ERR(sdev->regs);

	/* The module is clocked only while its parent is awake. */
	err = pm_runtime_resume_and_get(dev->parent);
	if (err)
		return err;
	err = pvrsgx_probe_common(sdev);
	pm_runtime_put(dev->parent);
	return err;
}

static void pvrsgx_of_remove(struct platform_device *pdev)
{
	pvrsgx_remove_common(platform_get_drvdata(pdev));
}

static void pvrsgx_of_shutdown(struct platform_device *pdev)
{
	pvrsgx_shutdown_common(platform_get_drvdata(pdev));
}

static const struct of_device_id pvrsgx_of_ids[] = {
	{ .compatible = "img,powervr-sgx530", .data = (void *)530 },
	{ }
};
MODULE_DEVICE_TABLE(of, pvrsgx_of_ids);

static struct platform_driver pvrsgx_of_driver = {
	.probe = pvrsgx_of_probe,
	.remove = pvrsgx_of_remove,
	.shutdown = pvrsgx_of_shutdown,
	.driver = {
		.name = PVRSGX_DRIVER_NAME,
		.of_match_table = pvrsgx_of_ids,
		.pm = pm_ptr(&pvrsgx_pm_ops),
	},
};

int pvrsgx_of_register(void)
{
	return platform_driver_register(&pvrsgx_of_driver);
}

void pvrsgx_of_unregister(void)
{
	platform_driver_unregister(&pvrsgx_of_driver);
}
