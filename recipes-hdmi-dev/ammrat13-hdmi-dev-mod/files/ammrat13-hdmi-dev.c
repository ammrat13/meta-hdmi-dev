#define pr_fmt(fmt) "ammrat13-hdmi-dev: " fmt
#define DEBUG

#include <asm/io.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>

#include <linux/device/driver.h>
#include <linux/platform_device.h>

#include <linux/dma-mapping.h>
#include <linux/fb.h>

#include <linux/interrupt.h>
#include <linux/jiffies.h>

/*******************************************************************************
 * Constants and Helper Functions
 ******************************************************************************/

static const off_t HDMI_CTRL_OFF = 0x00l;
static const off_t HDMI_GIE_OFF = 0x04l;
static const off_t HDMI_IER_OFF = 0x08l;
static const off_t HDMI_ISR_OFF = 0x0cl;
static const off_t HDMI_BUF_OFF = 0x10l;
static const off_t HDMI_COORD_DATA_OFF = 0x18l;
static const off_t HDMI_COORD_CTRL_OFF = 0x1cl;

static const size_t HDMI_MMIO_LEN = 0x20ul;
static const size_t HDMI_BUF_LEN = 640ul * 480ul * 4ul;
static const size_t HDMI_LINE_LEN = 640ul * 4ul;

/*
 * Bitmask for an interrupt that's fired on every VBlank. It's the mask into the
 * Interrupt Status Register and the Interrupt Enable Register.
 */
static const u32 HDMI_VBLANK_IRQ = 0x02ul;

static void hdmi_assert_types(void)
{
	BUILD_BUG_ON(sizeof(u8) != 1);
	BUILD_BUG_ON(sizeof(u32) != 4);
	BUILD_BUG_ON(sizeof(unsigned) <= 2);
	BUILD_BUG_ON(sizeof(unsigned long) != sizeof(u32));
	BUILD_BUG_ON(sizeof(void *) != sizeof(unsigned long));
	BUILD_BUG_ON(sizeof(void __iomem *) != sizeof(unsigned long));
	BUILD_BUG_ON(sizeof(dma_addr_t) != sizeof(unsigned long));
}

static void hdmi_assert_init(struct fb_info *info)
{
#ifdef DEBUG
	BUG_ON(info == NULL);
	BUG_ON(info->fix.mmio_start == 0ul);
	BUG_ON(info->fix.mmio_len != HDMI_MMIO_LEN);
	BUG_ON(info->fix.smem_start == 0ul);
	BUG_ON(info->fix.smem_len != HDMI_BUF_LEN);
	BUG_ON(info->screen_base == NULL);
	BUG_ON(info->screen_size != HDMI_BUF_LEN);
	BUG_ON(info->pseudo_palette == NULL);
	BUG_ON(info->fbops == NULL);
#endif /* DEBUG */
}

static void hdmi_assert_inbounds(off_t off)
{
#ifdef DEBUG
	BUG_ON(off < 0 || off >= HDMI_MMIO_LEN);
	BUG_ON(off % sizeof(u32) != 0);
#endif /* DEBUG */
}

static void hdmi_iowrite32(struct fb_info *info, off_t off, u32 val)
{
	hdmi_assert_init(info);
	hdmi_assert_inbounds(off);
	iowrite32(val, (void __iomem *)(info->fix.mmio_start + off));
}

static u32 hdmi_ioread32(struct fb_info *info, off_t off)
{
	hdmi_assert_init(info);
	hdmi_assert_inbounds(off);
	return ioread32((void __iomem *)(info->fix.mmio_start + off));
}

/*******************************************************************************
 * Coordinate and VBlank Handling
 ******************************************************************************/

/*
 * This is the internal representation of coordinates, which isn't necessarily
 * tied to hardware. It eventually gets turned into a `struct fb_vblank`.
 */
struct hdmi_coordinate {
	unsigned fid;
	unsigned row;
	unsigned col;
};

static struct hdmi_coordinate hdmi_coordinate_read(struct fb_info *info)
{
	struct hdmi_coordinate ret;
	u32 data;
	hdmi_assert_init(info);
	// Spin until the data is actually valid. This shouldn't take long -
	// just a few cycles.
	while ((hdmi_ioread32(info, HDMI_COORD_CTRL_OFF) & 1u) == 0u)
		;
	// Read and decode the data
	data = hdmi_ioread32(info, HDMI_COORD_DATA_OFF);
	ret.fid = (data >> 20) & 0xfffu;
	ret.row = (data >> 10) & 0x3ffu;
	ret.col = (data >> 0) & 0x3ffu;
	return ret;
}

static bool hdmi_coordinate_is_vblank(struct hdmi_coordinate coord)
{
	return coord.row < 45u;
}

static bool hdmi_coordinate_is_hblank(struct hdmi_coordinate coord)
{
	return coord.col < 160u;
}

static bool hdmi_coordinate_is_vsync(struct hdmi_coordinate coord)
{
	return coord.row >= 10u && coord.row < 12u;
}

/*******************************************************************************
 * Interrupt Handling
 ******************************************************************************/

/*
 * This wait queue is signaled on every VBlank by the ISR. All the threads
 * waiting on this MUST be interruptible, especially since it takes a long time
 * for the interrupts to come in.
 */
DECLARE_WAIT_QUEUE_HEAD(hdmi_vblank_waitq);

static irqreturn_t hdmi_isr(int irq, void *info_cookie)
{
	struct fb_info *info;
	u32 isr;

	// The routine establishing this IRQ handler MUST pass us the
	// `struct fb_info` data in the cookie.
	info = info_cookie;
	hdmi_assert_init(info);

	// Check to see if we even have an interrupt from this device
	if ((hdmi_ioread32(info, HDMI_CTRL_OFF) & 0x200u) == 0u)
		return IRQ_NONE;
	// If we do, read the Interrupt Status Register to find out what interrupts
	// we need to service. We should only have an interrupt for a new frame.
	isr = hdmi_ioread32(info, HDMI_ISR_OFF);
	BUG_ON(isr == 0);
	WARN_ON_ONCE(isr != HDMI_VBLANK_IRQ);

	wake_up_interruptible_all(&hdmi_vblank_waitq);
	hdmi_iowrite32(info, HDMI_ISR_OFF, isr);
	return IRQ_HANDLED;
}

/*******************************************************************************
 * Framebuffer Structures
 ******************************************************************************/

static int hdmi_setcolreg(unsigned regno, unsigned red, unsigned green,
			  unsigned blue, unsigned transp, struct fb_info *info);
static int hdmi_check_var(struct fb_var_screeninfo *var, struct fb_info *info);
static int hdmi_mmap(struct fb_info *info, struct vm_area_struct *vma);
static int hdmi_ioctl(struct fb_info *info, unsigned int cmd,
		      unsigned long arg);

__maybe_unused static int hdmi_set_par(struct fb_info *info);

static struct fb_fix_screeninfo hdmi_fix_init = {
	/*
	 * Still have to set:
	 *   * `.smem_start`
	 *   * `.mmio_start`
	 */
	.id = "ammrat13-fb",
	.smem_len = HDMI_BUF_LEN,
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_TRUECOLOR,
	.xpanstep = 0,
	.ypanstep = 0,
	.ywrapstep = 0,
	.line_length = HDMI_LINE_LEN,
	.mmio_len = HDMI_MMIO_LEN,
	.accel = FB_ACCEL_NONE,
	.capabilities = 0,
};

static struct fb_var_screeninfo hdmi_var_init = {
	/*
	 * Timing fields are taken from: drivers/video/fbdev/core/modedb.c.
	 * Specifically, see the entries in `modedb`, and look at the conversion
	 * in `fb_videomode_to_var`.
	 */
	.xres = 640,
	.yres = 480,
	.xres_virtual = 640,
	.yres_virtual = 480,
	.xoffset = 0,
	.yoffset = 0,
	.bits_per_pixel = 32,
	.grayscale = 0,
	.red = { .offset = 16, .length = 8, .msb_right = 0 },
	.green = { .offset = 8, .length = 8, .msb_right = 0 },
	.blue = { .offset = 0, .length = 8, .msb_right = 0 },
	.transp = { .offset = 24, .length = 0, .msb_right = 0 },
	.nonstd = 0,
	.height = -1,
	.width = -1,
	.pixclock = 39721u,
	.left_margin = 40u,
	.right_margin = 24u,
	.upper_margin = 32u,
	.lower_margin = 11u,
	.hsync_len = 96u,
	.vsync_len = 2u,
	.sync = FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	.vmode = FB_VMODE_NONINTERLACED,
};

static struct fb_ops hdmi_fbops = {
	.owner = THIS_MODULE,
	/* .fb_open is uneeded because we don't do user multiplexing */
	/* .fb_release is uneeded because we don't do user multiplexing */
	.fb_read = fb_sys_read,
	.fb_write = fb_sys_write,
	.fb_check_var = hdmi_check_var,
#ifdef DEBUG
	.fb_set_par = hdmi_set_par,
#endif
	.fb_setcolreg = hdmi_setcolreg,
	/* .fb_setcmap iteratively calls .fb_setcolreg by default */
	/* .fb_blank errors by default */
	/* .fb_pan_display errors by default */
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	/* .fb_cursor uses a software cursor by default */
	/* .fb_sync is a no-op by default */
	.fb_ioctl = hdmi_ioctl,
	.fb_mmap = hdmi_mmap,
	/* .fb_destroy does nothing special by default */
};

/*******************************************************************************
 * Framebuffer Operations
 *
 * Note that these were forward-declared in the previous section. Make sure to
 * add them there to register them in `hdmi_fbops`.
 ******************************************************************************/

/*
 * For true-color mode, the kernel expects us to allocate and manage a pseudo
 * palette. This is the function the kernel uses to set entries in that. We
 * allocated it in the probe function.
 */
static int hdmi_setcolreg(unsigned regno, unsigned red, unsigned green,
			  unsigned blue, unsigned transp, struct fb_info *info)
{
	/*
	 * The inputs to this function are 16-bit, so convert to 8-bit. The
	 * conversion here isn't just a simple divide by 256, though that would
	 * work. The actual ratio is (2**16 - 1) / (2**8 - 1). The formula below
	 * is used elsewhere in the kernel to get the true closest answer for
	 * that ratio.
	 *
	 * Also note that we fix the parameters. Other places in the kernel
	 * dynamically compute values from `info`, but our stuff is always
	 * fixed, so we can just use the fixed values.
	 */
#define CNVT_TOHW(val, width) ((((val) << (width)) + 0x7fff - (val)) >> 16)
	red = CNVT_TOHW(red, 8);
	green = CNVT_TOHW(green, 8);
	blue = CNVT_TOHW(blue, 8);
	transp = CNVT_TOHW(transp, 8);
#undef CNVT_TOHW

#if 0
	// This has a tendancy to spam the log, so we disable it. The checks
	// should still happen after the print, though.
	pr_debug("setting color register %u to (%u, %u, %u, %u)\n", regno, red,
		 green, blue, transp);
#endif
	if (WARN_ON(info == NULL))
		return 1;
	hdmi_assert_init(info);

	// The pseudo palette is expected to be 16 entries long, and that's
	// exactly what we allocated
	if (regno >= 16)
		return 1;

	// The fields here MUST match what's set in `info->var`
	((u32 *)info->pseudo_palette)[regno] = (red << 16) | (green << 8) |
					       (blue << 0);
	return 0;
}

/*
 * This function gates user changes to the framebuffer geometry. The hardware
 * only supports one configuration, though. So, we check if the thing passed
 * in is close enough, modifying it if it is and erroring otherwise.
 */
static int hdmi_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	pr_info("called check_var for %p on %p\n", var, info);
	if (WARN_ON(var == NULL || info == NULL))
		return -EINVAL;
	hdmi_assert_init(info);

	// It appears that we're responsible for rounding up impossible values
	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	if (var->yres_virtual < var->yres)
		var->yres_virtual = var->yres;

	// The resolution is fixed by the hardware, ...
	if (var->xres != 640 || var->yres != 480) {
		pr_info("-> resolution mismatch\n");
		return -EINVAL;
	}
	// ... as is the virtual resolution, ...
	if (var->xres_virtual != 640 || var->yres_virtual != 480) {
		pr_info("-> virtual resolution mismatch\n");
		return -EINVAL;
	}
	// ... the buffer structure, ...
	if ((var->vmode & FB_VMODE_MASK) != FB_VMODE_NONINTERLACED) {
		pr_info("-> incorrect buffer structure\n");
		return -EINVAL;
	}
	// ... and the color depth.
	if (var->bits_per_pixel != 32 || var->grayscale != 0) {
		pr_info("-> color depth mismatch\n");
		return -EINVAL;
	}
	// We don't support hardware panning.
	if (var->xoffset != 0 || var->yoffset != 0) {
		pr_info("-> panning not supported\n");
		return -EINVAL;
	}

	/*
	 * If the request is close enough, modify the rest of the fields to
	 * match what we actually have. Note that this doesn't touch:
	 *  * `.activate`, nor
	 *  * `.rotate` since that's handled in software.
	 */
	{
		var->red = hdmi_var_init.red;
		var->green = hdmi_var_init.green;
		var->blue = hdmi_var_init.blue;
		var->transp = hdmi_var_init.transp;
		var->nonstd = hdmi_var_init.nonstd;

		var->pixclock = hdmi_var_init.pixclock;
		var->left_margin = hdmi_var_init.left_margin;
		var->right_margin = hdmi_var_init.right_margin;
		var->upper_margin = hdmi_var_init.upper_margin;
		var->lower_margin = hdmi_var_init.lower_margin;
		var->hsync_len = hdmi_var_init.hsync_len;
		var->vsync_len = hdmi_var_init.vsync_len;
		var->sync = hdmi_var_init.sync;

		// The mode field is used both for interlacing and how the
		// console should be updated. Only update the interlacing bit.
		var->vmode = (hdmi_var_init.vmode & FB_VMODE_MASK) |
			     (var->vmode & ~FB_VMODE_MASK);
	}
	return 0;
}

/*
 * This function is used to map the framebuffer into the user's address space.
 * By default, the framebuffer is treated as IO memory, but we want a weak
 * memory ordering.
 */
static int hdmi_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	pr_info("called mmap for %p on %p\n", vma, info);
	if (WARN_ON(info == NULL))
		return -EINVAL;
	hdmi_assert_init(info);

	return dma_mmap_attrs(info->dev, vma, info->screen_base,
			      info->fix.smem_start, info->fix.smem_len,
			      DMA_ATTR_WRITE_COMBINE);
}

/*
 * We support VBlanks, and we should try to expose that to user-space. It seems
 * the way this is usually done is through ioctls, specifically `FBIOGET_VBLANK`
 * and `FBIO_WAITFORVSYNC`. We implement both.
 */
static int hdmi_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	if (WARN_ON(info == NULL))
		return -EINVAL;
	hdmi_assert_init(info);

	switch (cmd) {
	case FBIOGET_VBLANK: {
		struct fb_vblank ret;
		struct hdmi_coordinate coord;
#if 0
		// This could spam the log since it could be called on every
		// frame, so we disable it
		pr_debug("called ioctl(FBIOGET_VBLANK) on %p\n", info);
#endif
		memset(&ret, 0, sizeof(ret));
		ret.flags = FB_VBLANK_HAVE_VBLANK | FB_VBLANK_HAVE_HBLANK |
			    FB_VBLANK_HAVE_COUNT | FB_VBLANK_HAVE_VCOUNT |
			    FB_VBLANK_HAVE_HCOUNT | FB_VBLANK_HAVE_VSYNC;

		coord = hdmi_coordinate_read(info);
		ret.count = coord.fid;
		ret.vcount = coord.row;
		ret.hcount = coord.col;
		if (hdmi_coordinate_is_vblank(coord))
			ret.flags |= FB_VBLANK_VBLANKING;
		if (hdmi_coordinate_is_hblank(coord))
			ret.flags |= FB_VBLANK_HBLANKING;
		if (hdmi_coordinate_is_vsync(coord))
			ret.flags |= FB_VBLANK_VSYNCING;

		BUILD_BUG_ON(sizeof(ret) == 0);
		if (copy_to_user((void __user *)arg, &ret, sizeof(ret)))
			return -EFAULT;
		return 0;
	}

	case FBIO_WAITFORVSYNC: {
		int res;
#if 0
		// This could spam the log since it could be called on every
		// frame, so we disable it
		pr_debug("called ioctl(FBIO_WAITFORVSYNC) on %p\n", info);
#endif
		// Each frame is just under 17ms. We give a 20% margin. If we
		// don't hear back by then, something is wrong.
		res = wait_event_interruptible_timeout(
			hdmi_vblank_waitq,
			hdmi_coordinate_is_vblank(hdmi_coordinate_read(info)),
			msecs_to_jiffies(20));
		if (res == -ERESTARTSYS)
			return -EINTR;
		return WARN_ON(res == 0) ? -ETIMEDOUT : 0;
	}

	default: {
		pr_info("called unsupported ioctl(%u) on %p\n", cmd, info);
		return -ENOTTY;
	}
	}
}

/*
 * The default for this function is a no-op, which makes sense for us since we
 * have no hardware to configure. However, we'll use this opportunity to do an
 * extra test. We should never try to set the hardware to a state that wouldn't
 * pass `check_var`.
 */
__maybe_unused static int hdmi_set_par(struct fb_info *info)
{
	struct fb_var_screeninfo new_var;

	pr_info("called set_par on %p\n", info);
	if (WARN_ON(info == NULL))
		return 1;
	hdmi_assert_init(info);

	new_var = info->var;
	return hdmi_check_var(&new_var, info) != 0;
}

/*******************************************************************************
 * Device Setup and Teardown
 ******************************************************************************/

/*
 * Helper function to allocate a `struct fb_info`. It also initializes the
 * structure with default/template values. This allocation is unmanaged, and
 * it is the caller's responsibility to release.
 */
static int hdmi_probe_create_fbinfo(struct platform_device *pdev,
				    struct fb_info **info)
{
	*info = framebuffer_alloc(0, &pdev->dev);
	if (*info == NULL) {
		pr_err("failed to allocate framebuffer device\n");
		return -ENOMEM;
	}

	/*
	 * Still need to set:
	 *   * `.screen_base`
	 */
	pr_info("allocated framebuffer device @ %p\n", *info);
	(*info)->fix = hdmi_fix_init;
	(*info)->var = hdmi_var_init;
	(*info)->fbops = &hdmi_fbops;
	(*info)->screen_size = (*info)->fix.smem_len;
	return 0;
}

/*
 * Helper function to map the device registers into our address space. It puts
 * the virtual address in the `mmio_start` field of the framebuffer info.
 */
static int hdmi_probe_map_registers(struct platform_device *pdev,
				    struct fb_info *info)
{
	struct resource *res;
	void __iomem *reg;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	reg = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(reg)) {
		pr_err("failed to map registers\n");
		return PTR_ERR(reg);
	}

	pr_debug("mapped registers @ %p\n", reg);
	info->fix.mmio_start = (unsigned long)reg;
	return 0;
}

/*
 * Helper function to allocate the frame buffer in DMA memory. It puts both
 * the virtual and bus addresses of the buffer into the `struct fb_info`.
 *
 * The buffer doesn't have to be physically contiguous in memory, as long as
 * its contiguous in bus memory. The kernel will use the IOMMU to ensure this,
 * or it will allocate it contiguously.
 *
 * Finally, we allow store buffer optimizations on the buffer. Really, we can
 * go down to a weak memory ordering since it's write only, but that's
 * actually not implemented on ARM.
 */
static int hdmi_probe_alloc_buffer(struct platform_device *pdev,
				   struct fb_info *info)
{
	void *vir_addr;
	dma_addr_t bus_addr;

	vir_addr = dmam_alloc_attrs(&pdev->dev, HDMI_BUF_LEN, &bus_addr,
				    GFP_KERNEL, DMA_ATTR_WRITE_COMBINE);
	if (!vir_addr) {
		pr_err("failed to allocate buffer\n");
		return -ENOMEM;
	}

	pr_debug("allocated buffer @ %p (bus: %x)\n", vir_addr, bus_addr);
	info->screen_base = (void __force *)vir_addr;
	info->fix.smem_start = bus_addr;
	return 0;
}

/*
 * In true-color mode, the kernel expects us to allocate a pseudo palette. This
 * maps sixteen colors to their corresponding 32-bit values.
 */
static int hdmi_probe_alloc_pseudo_palette(struct platform_device *pdev,
					   struct fb_info *info)
{
	u32 *palette;

	palette = devm_kzalloc(&pdev->dev, 16 * sizeof(u32), GFP_KERNEL);
	if (!palette) {
		pr_err("failed to allocate pseudo palette\n");
		return -ENOMEM;
	}

	pr_debug("allocated pseudo palette @ %p\n", palette);
	info->pseudo_palette = palette;
	return 0;
}

/*
 * Helper function to request the IRQ for the device. It registers the function
 * `hdmi_isr`, and passes it the `struct fb_info` as the cookie. Note that
 * interrupts will not happen until the device is started.
 */
static int hdmi_probe_request_irq(struct platform_device *pdev,
				  struct fb_info *info)
{
	int irq;
	int res;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		pr_err("failed to get IRQ\n");
		return irq;
	}

	res = devm_request_irq(&pdev->dev, irq, hdmi_isr, 0,
			       "ammrat13-hdmi-dev", info);
	if (res < 0) {
		pr_err("failed to request IRQ\n");
		return res;
	}

	pr_debug("registered handler for IRQ %d\n", irq);
	return 0;
}

/*
 * Helper function to register the framebuffer device with the kernel. At this
 * point, the `struct fb_info` MUST be fully initialized.
 */
static int hdmi_probe_register_fbinfo(struct fb_info *info)
{
	int res;
	hdmi_assert_init(info);

	if ((res = register_framebuffer(info)) < 0) {
		pr_err("failed to register framebuffer device\n");
		return res;
	}
	pr_debug("registered framebuffer device\n");
	return 0;
}

static int hdmi_probe(struct platform_device *pdev)
{
	struct fb_info *info;
	int res;
	hdmi_assert_types();

	pr_info("called probe on %p\n", pdev);
	if (WARN_ON(pdev == NULL))
		return -EINVAL;

	/*
	 * The driver data is a `struct fb_info`. The allocation for it is
	 * unmanaged. This is done for two reasons:
	 *   1. We need special handling for registration, and that sort of goes
	 *      hand-in-hand with allocation.
	 *   2. The `devres` subsystem is built on a stack. If we used the
	 *      `devres` subsystem, we'd have to allocate this last to prevent
	 *      the things it references from being freed first.
	 */
	if ((res = hdmi_probe_create_fbinfo(pdev, &info)) != 0)
		goto err;
	BUG_ON(info == NULL);

	/*
	 * Call all of the initialization functions. These may have dependencies
	 * on each other, so the order in which we call them matters. If any of
	 * them fail, make sure to clean up after them.
	 */
	if ((res = hdmi_probe_map_registers(pdev, info)) != 0)
		goto err;
	if ((res = hdmi_probe_alloc_buffer(pdev, info)) != 0)
		goto err;
	if ((res = hdmi_probe_alloc_pseudo_palette(pdev, info)) != 0)
		goto err;
	if ((res = hdmi_probe_request_irq(pdev, info)) != 0)
		goto err;
	if ((res = hdmi_probe_register_fbinfo(info)) != 0)
		goto err;

	// Tell the device the buffer address
	hdmi_iowrite32(info, HDMI_BUF_OFF, info->fix.smem_start);
	// Enable interrupts on VBlank
	hdmi_iowrite32(info, HDMI_GIE_OFF, 0x01ul);
	hdmi_iowrite32(info, HDMI_IER_OFF, HDMI_VBLANK_IRQ);
	// Clear the coordinate valid bit from the previous run (if any)
	hdmi_ioread32(info, HDMI_COORD_CTRL_OFF);
	// Start the device
	hdmi_iowrite32(info, HDMI_CTRL_OFF, 0x081ul);

	dev_set_drvdata(&pdev->dev, info);
	return 0;

err:
	framebuffer_release(info);
	return res;
}

static int hdmi_remove(struct platform_device *pdev)
{
	/*
	 * When we were probing this device, we did our best to use managed
	 * resources. This means they will be cleaned up automatically when this
	 * function returns. We just have to deal with the non-managed
	 * resources, i.e. the `struct fb_info`.
	 *
	 * Also, we know that the device was successfully probed if we made it
	 * here. The `remove` function is not called on probe failure.
	 *
	 * The mainline kernel says that the return value from this function is
	 * ignored. So, we always return 0. It also means we have to do our own
	 * error handling if needed.
	 */
	struct fb_info *info;

	pr_info("called remove on %p\n", pdev);
	if (WARN_ON(pdev == NULL))
		return -EINVAL;

	info = dev_get_drvdata(&pdev->dev);
	hdmi_assert_init(info);

	// First and foremost, stop the device
	hdmi_iowrite32(info, HDMI_CTRL_OFF, 0x000ul);
	// Disable interrupts for the next guy
	hdmi_iowrite32(info, HDMI_GIE_OFF, 0x00ul);
	hdmi_iowrite32(info, HDMI_IER_OFF, 0x00ul);
	// Clear the coordinate valid bit from this run. Not strictly necessary,
	// but just in case.
	hdmi_ioread32(info, HDMI_COORD_CTRL_OFF);
	// Note that we keep the buffer address in the device. The next driver should
	// treat it as garbage, but it will allocate a new one.

	// The `struct fb_info` is not managed, so we have to free it ourselves. To do
	// so, we have to unregister then release - one is not enough.
	pr_info("freeing framebuffer device @ %p\n", info);
	unregister_framebuffer(info);
	framebuffer_release(info);
	// Set all references to the `struct fb_info` to NULL for safety
	dev_set_drvdata(&pdev->dev, NULL);
	info = NULL;

	return 0;
}

/*******************************************************************************
 * Module Registration
 ******************************************************************************/

static struct of_device_id hdmi_match[] = {
	// Names for the `.compatible`` field are taken from the final device tree
	{ .compatible = "xlnx,hdmi-cmd-gen-0.0" },
	{ .compatible = "xlnx,hdmi-cmd-gen" },
	{ /* null-terminator */ },
};
static struct platform_driver hdmi_driver = {
    .probe = hdmi_probe,
    .remove = hdmi_remove,
    .driver =
        {
            .name = "ammrat13-hdmi-dev",
            .owner = THIS_MODULE,
            .of_match_table = hdmi_match,
        },
};
module_platform_driver(hdmi_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ammar Ratnani <ammrat13@gmail.com>");
MODULE_DESCRIPTION("Kernel module to drive ammrat13's HDMI Peripheral");
