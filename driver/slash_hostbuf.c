/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if
 * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/*
 * DMA-coherent host memory addressable by a kernel's AXI master.
 *
 * Kernels reach host DRAM through the QDMA slave bridge, which addresses it by
 * bus address rather than CPU virtual address. Userspace needs both views of
 * the same bytes: a mapping to write through, and the bus address to hand the
 * device. The allocation is coherent, so a design can keep a command ring here
 * and have the device pull from it without an explicit sync on either side.
 *
 * This differs from slash_dmabuf.c, which exports device BAR regions; here the
 * pages are host memory owned by this driver.
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "slash.h"
#include "slash_dmabuf.h"

/**
 * struct slash_hostbuf - One coherent host allocation exported to userspace.
 * @pdev:     Owning PCI device; referenced for the lifetime of the buffer.
 * @cpu_addr: Kernel virtual address returned by dma_alloc_coherent().
 * @dma_addr: Bus address the device masters to.
 * @len:      Page-rounded allocation size.
 */
struct slash_hostbuf {
    struct pci_dev *pdev;
    void           *cpu_addr;
    dma_addr_t      dma_addr;
    size_t          len;
};

static int slash_hostbuf_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
    /*
     * Importing into another device would need a second mapping of these
     * pages; the slave bridge address handed out here would not describe it.
     */
    dev_warn(attach->dev, "%s: device attachments are not supported for host buffers", SLASH_NAME);
    return -EOPNOTSUPP;
}

static struct sg_table *slash_hostbuf_map(struct dma_buf_attachment *attach,
                                          enum dma_data_direction dir)
{
    return ERR_PTR(-EOPNOTSUPP);
}

static void slash_hostbuf_unmap(struct dma_buf_attachment *attach, struct sg_table *sgt,
                                enum dma_data_direction dir)
{
}

static int slash_hostbuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
    struct slash_hostbuf *buf = dmabuf->priv;
    size_t size = vma->vm_end - vma->vm_start;

    if (vma->vm_pgoff != 0) {
        return -EINVAL;
    }
    if (size > buf->len) {
        return -EINVAL;
    }

    /*
     * dma_mmap_coherent installs the same attributes the allocation was made
     * with, which is what keeps the userspace view coherent with the device's.
     */
    return dma_mmap_coherent(&buf->pdev->dev, vma, buf->cpu_addr, buf->dma_addr, size);
}

static void slash_hostbuf_release(struct dma_buf *dmabuf)
{
    struct slash_hostbuf *buf = dmabuf->priv;

    dma_free_coherent(&buf->pdev->dev, buf->len, buf->cpu_addr, buf->dma_addr);
    pci_dev_put(buf->pdev);
    kfree(buf);
}

static const struct dma_buf_ops slash_hostbuf_ops = {
    .attach   = slash_hostbuf_attach,
    .map_dma_buf   = slash_hostbuf_map,
    .unmap_dma_buf = slash_hostbuf_unmap,
    .mmap     = slash_hostbuf_mmap,
    .release  = slash_hostbuf_release,
};

/**
 * slash_hostbuf_create() - Allocate coherent host memory for device mastering.
 * @pdev:     Owning PCI device.
 * @length:   Requested size in bytes; rounded up to a page.
 * @dma_addr: Receives the bus address the device masters to.
 * @out_len:  Receives the granted (rounded) length.
 *
 * Return: an owning dma_buf on success, ERR_PTR otherwise.
 */
struct dma_buf *slash_hostbuf_create(struct pci_dev *pdev, u64 length,
                                     dma_addr_t *dma_addr, size_t *out_len)
{
    DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
    struct slash_hostbuf *buf;
    struct dma_buf *dmabuf;
    size_t len;
    long err;

    if (length == 0) {
        return ERR_PTR(-EINVAL);
    }

    len = PAGE_ALIGN(length);

    buf = kzalloc(sizeof(*buf), GFP_KERNEL);
    if (!buf) {
        return ERR_PTR(-ENOMEM);
    }

    buf->pdev = pci_dev_get(pdev);
    buf->len  = len;
    buf->cpu_addr = dma_alloc_coherent(&pdev->dev, len, &buf->dma_addr, GFP_KERNEL);
    if (!buf->cpu_addr) {
        dev_err(&pdev->dev, "%s: dma_alloc_coherent(%zu) failed\n", SLASH_NAME, len);
        err = -ENOMEM;
        goto err_put;
    }

    exp_info.ops      = &slash_hostbuf_ops;
    exp_info.size     = len;
    exp_info.flags    = O_RDWR;
    exp_info.priv     = buf;
    exp_info.exp_name = SLASH_NAME;

    dmabuf = dma_buf_export(&exp_info);
    if (IS_ERR(dmabuf)) {
        err = PTR_ERR(dmabuf);
        dev_err(&pdev->dev, "%s: dma_buf_export failed: %ld\n", SLASH_NAME, err);
        goto err_free_dma;
    }

    *dma_addr = buf->dma_addr;
    *out_len  = len;

    dev_dbg(&pdev->dev, "%s: host buffer len=%zu dma=%pad\n", SLASH_NAME, len, &buf->dma_addr);
    return dmabuf;

err_free_dma:
    dma_free_coherent(&pdev->dev, len, buf->cpu_addr, buf->dma_addr);
err_put:
    pci_dev_put(buf->pdev);
    kfree(buf);
    return ERR_PTR(err);
}
