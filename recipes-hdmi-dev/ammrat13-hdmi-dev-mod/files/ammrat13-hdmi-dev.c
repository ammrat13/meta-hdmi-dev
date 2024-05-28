#define pr_fmt(fmt) "ammrat13-hdmi-dev: " fmt

#include <linux/module.h>

#include <asm/io.h>
#include <linux/device/driver.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

// -----------------------------------------------------------------------------
// HDMI Driver Data

// Structure describing an HDMI Peripheral. We allocate one of these on probe,
// and we stick it in the `driver_data` field of the device.
struct hdmi_driver_data {

  // The MMIO region for the device, mapped into our virtual address space
  void __iomem *registers;

  // The virtual address of the framebuffer memory
  u32 *buf_virt;
  // The bus address of the framebuffer memory
  dma_addr_t buf_bus;
};

// Byte offsets for all the registers
#define HDMI_CTRL_OFF 0x00
#define HDMI_GIE_OFF 0x04
#define HDMI_IER_OFF 0x08
#define HDMI_ISR_OFF 0x0c
#define HDMI_FRAMEBUF_OFF 0x10
#define HDMI_COORD_DATA_OFF 0x18
#define HDMI_COORD_CTRL_OFF 0x1c

// The size of the framebuffer memory in words and in bytes. Also, how much we
// should allocate.
#define HDMI_BUF_LEN_WORDS (640 * 480)
#define HDMI_BUF_LEN_BYTES (HDMI_BUF_LEN_WORDS * 4)
#define HDMI_BUF_ALLOC_BYTES (PAGE_ALIGN(HDMI_BUF_LEN_BYTES))

// -----------------------------------------------------------------------------
// HDMI Platform Driver

static irqreturn_t hdmi_irq_handler(int irq, void *ddata_cookie) {

  // Pointer to this device's driver data on the heap
  struct hdmi_driver_data *ddata;
  // The interrupts we have to service
  u32 isr;

  // Extract the driver data from the cookie
  ddata = ddata_cookie;

  // Check to see if we even have an interrupt from this device
  if ((ioread32(ddata->registers + HDMI_CTRL_OFF) & 0x200) == 0)
    return IRQ_NONE;
  // If we do, read the ISR's contents
  isr = ioread32(ddata->registers + HDMI_ISR_OFF);

  // Acknowlege all the present interrupts
  iowrite32(isr, ddata->registers + HDMI_ISR_OFF);
  return IRQ_HANDLED;
}

int hdmi_probe(struct platform_device *pdev) {

  // Pointer to this device's driver data on the heap
  struct hdmi_driver_data *ddata;

  // Log and initialize variables
  pr_info("called probe on %p\n", pdev);
  ddata = NULL;

  // Allocate the driver data and set it in the `struct device`
  {
    // Try to allocate the driver data
    ddata = devm_kzalloc(&pdev->dev, sizeof(*ddata), GFP_KERNEL);
    if (!ddata) {
      pr_err("failed to allocate driver data\n");
      return -ENOMEM;
    }
    // It it worked, update the `struct device`
    dev_set_drvdata(&pdev->dev, ddata);
    pr_info("allocated driver data @ %p\n", ddata);
    BUG_ON((unsigned long)dev_get_drvdata(&pdev->dev) != (unsigned long)ddata);
  }

  // Initialize everything to `NULL`
  ddata->registers = NULL;
  ddata->buf_virt = NULL;
  ddata->buf_bus = 0;

  // Get the registers for this device. Map it into our address space and store
  // the virtual address.
  {
    struct resource *res;
    void __iomem *reg;
    // Get the resource and map it
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    reg = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(reg)) {
      pr_err("failed to map registers\n");
      return PTR_ERR(reg);
    }
    // Now update
    ddata->registers = reg;
    pr_info("mapped registers @ %p\n", reg);
  }

  // Register the IRQ handler
  {
    int irq;
    int res;
    // Check that the IRQ exists
    irq = platform_get_irq(pdev, 0);
    if (irq < 0) {
      pr_err("failed to get IRQ\n");
      return irq;
    }
    // Register the IRQ handler. Pass it the driver data as the cookie. Note
    // that this won't see interrupts until we start the device.
    res = devm_request_irq(&pdev->dev, irq, hdmi_irq_handler, 0,
                           "ammrat13-hdmi-dev", ddata);
    if (res < 0) {
      pr_err("failed to request IRQ\n");
      return res;
    }
    // Log success
    pr_info("registered handler for IRQ %d\n", irq);
  }

  // Allocate the framebuffer in DMA memory
  {
    void *v;
    dma_addr_t b;
    // Do the allocation. We allow write combining. Also, we could force the
    // allocation to be contiguous. However, it should be fine because the
    // kernel will return a contiguous bus address space, which is really what
    // we need.
    v = dmam_alloc_attrs(&pdev->dev, HDMI_BUF_ALLOC_BYTES, &b, GFP_KERNEL,
                         DMA_ATTR_WRITE_COMBINE);
    if (!v) {
      pr_err("failed to allocate framebuffer\n");
      return -ENOMEM;
    }
    // Update the driver data
    ddata->buf_virt = v;
    ddata->buf_bus = b;
    pr_info("allocated framebuffer @ %p (bus: %x)\n", v, b);
  }

  // Enable interrupts
  iowrite32(0x01, ddata->registers + HDMI_GIE_OFF);
  iowrite32(0x03, ddata->registers + HDMI_IER_OFF);
  // Start the device
  iowrite32(0x081, ddata->registers + HDMI_CTRL_OFF);

  return 0;
}

int hdmi_remove(struct platform_device *pdev) {
  // When we were probing this device, we did our best to use managed resources.
  // This means they will be cleaned up automatically when this function
  // returns. We just have to deal with the non-managed resources.
  //
  // Also, we know that the device was successfully probed if we made it here.
  // The `remove` function is not called on probe failure.
  //
  // The mainline kernel says that the return value from this function is
  // ignored. So, we always return 0. It also means we have to do our own error
  // handling if needed.

  // Pointer to this device's driver data on the heap
  struct hdmi_driver_data *ddata;

  // Log and initialize variables
  pr_info("called remove on %p\n", pdev);
  ddata = dev_get_drvdata(&pdev->dev);
  // We should not have uninitialized driver data
  BUG_ON(!ddata);
  BUG_ON(!ddata->registers);
  BUG_ON(!ddata->buf_virt);
  BUG_ON(!ddata->buf_bus);

  // First and foremost, stop the device
  iowrite32(0x000, ddata->registers + HDMI_CTRL_OFF);
  // Disable interrupts
  iowrite32(0x00, ddata->registers + HDMI_GIE_OFF);
  iowrite32(0x00, ddata->registers + HDMI_IER_OFF);

  return 0;
}

// -----------------------------------------------------------------------------
// Module Registration

// Match table for which devices in the device tree to probe with this driver.
// This is included in the `struct platform_driver` below. The names for the
// `compatible` field are taken from the final device tree itself.
static struct of_device_id hdmi_match[] = {
    {.compatible = "xlnx,hdmi-cmd-gen-0.0"},
    {.compatible = "xlnx,hdmi-cmd-gen"},
    {/* null-terminator */},
};
// Structure describing this driver. This will be passed to the kernel for
// matching on the device tree.
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
// This macro defines the `init` and `exit` functions for the module. They just
// forward to the `probe` and `remove` functions of the driver.
module_platform_driver(hdmi_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ammar Ratnani <ammrat13@gmail.com>");
MODULE_DESCRIPTION("Kernel module to drive ammrat13's HDMI Peripheral");
