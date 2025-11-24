// axi_dma_map.c
#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>

#define DEFAULT_DMA_REG_BASE 0x40400000      /* AXI DMA register base */
#define DEFAULT_DMA_REG_SIZE 0x00010000      /* 64 KiB window */

static unsigned long dma_reg_base = DEFAULT_DMA_REG_BASE;
module_param(dma_reg_base, ulong, 0444);
MODULE_PARM_DESC(dma_reg_base, "Physical base address to expose via /dev/axi_dma_regs");

static unsigned int dma_reg_size = DEFAULT_DMA_REG_SIZE;
module_param(dma_reg_size, uint, 0444);
MODULE_PARM_DESC(dma_reg_size, "Register window size in bytes");

static void __iomem *dma_regs;

static ssize_t axi_dma_read(struct file *file, char __user *buf,
                            size_t len, loff_t *ppos)
{
    if (*ppos < 0 || *ppos + len > dma_reg_size)
        return -EINVAL;
    if (!dma_regs)
        return -ENODEV;

    if (copy_to_user(buf, (void __force *)(dma_regs + *ppos), len))
        return -EFAULT;

    pr_debug("axi_dma_map: read %zu bytes @ +0x%llx\n", len, *ppos);
    *ppos += len;
    return len;
}

static ssize_t axi_dma_write(struct file *file, const char __user *buf,
                             size_t len, loff_t *ppos)
{
    if (*ppos < 0 || *ppos + len > dma_reg_size)
        return -EINVAL;
    if (!dma_regs)
        return -ENODEV;

    if (copy_from_user((void __force *)(dma_regs + *ppos), buf, len))
        return -EFAULT;

    pr_debug("axi_dma_map: wrote %zu bytes @ +0x%llx\n", len, *ppos);
    *ppos += len;
    return len;
}

static const struct file_operations axi_dma_fops = {
    .owner  = THIS_MODULE,
    .read   = axi_dma_read,
    .write  = axi_dma_write,
    .llseek = default_llseek,
};

static struct miscdevice axi_dma_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "axi_dma_regs",
    .fops  = &axi_dma_fops,
};

static int __init axi_dma_map_init(void)
{
    if (dma_reg_size == 0) {
        pr_err("axi_dma_map: dma_reg_size must be > 0\n");
        return -EINVAL;
    }

    dma_regs = ioremap(dma_reg_base, dma_reg_size);
    if (!dma_regs) {
        pr_err("axi_dma_map: ioremap failed for 0x%08lx\n", dma_reg_base);
        return -ENOMEM;
    }
    pr_info("axi_dma_map: mapped 0x%08lx–0x%08lx as /dev/axi_dma_regs\n",
            dma_reg_base, dma_reg_base + dma_reg_size - 1);
    return misc_register(&axi_dma_misc);
}

static void __exit axi_dma_map_exit(void)
{
    misc_deregister(&axi_dma_misc);
    if (dma_regs)
        iounmap(dma_regs);
    dma_regs = NULL;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Expose AXI DMA registers via /dev/axi_dma_regs");
MODULE_VERSION("1.1");

module_init(axi_dma_map_init);
module_exit(axi_dma_map_exit);
