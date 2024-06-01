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
// and we stick it in the `driver_data` field of the device. This way, we can
// access the device's information from all the functions that are called on it.
struct hdmi_driver_data {

  // The MMIO region for the device, mapped into our virtual address space. This
  // will not be the same as the physical address of the registers.
  void __iomem *registers;

  // The virtual and bus addresses of the framebuffer memory. Remember that bus
  // addresses are the addresses that the device sees. On our platform, they are
  // the same as physical addresses, but that isn't true with an IOMMU.
  u32 *buf_virt;
  dma_addr_t buf_bus;
};

static void hdmi_drvdata_assert_init(struct hdmi_driver_data *drvdata) {
  // Helper function to assert that the driver data is initialized. If it is not
  // completely initialized, this function causes a kernel panic.
  BUG_ON(!drvdata);
  BUG_ON(!drvdata->registers);
  BUG_ON(!drvdata->buf_virt);
  BUG_ON(!drvdata->buf_bus);
}

static const off_t HDMI_CTRL_OFF = 0x00;
static const off_t HDMI_GIE_OFF = 0x04;
static const off_t HDMI_IER_OFF = 0x08;
static const off_t HDMI_ISR_OFF = 0x0c;
static const off_t HDMI_BUF_OFF = 0x10;
static const off_t HDMI_COORD_DATA_OFF = 0x18;
static const off_t HDMI_COORD_CTRL_OFF = 0x1c;

static const size_t HDMI_BUF_LEN_WORDS = 640ul * 480ul;
static const size_t HDMI_BUF_LEN_BYTES = HDMI_BUF_LEN_WORDS * 4ul;

// -----------------------------------------------------------------------------
// HDMI Platform Driver

static irqreturn_t hdmi_irq_handler(int irq, void *drvdata_cookie) {
  struct hdmi_driver_data *drvdata;
  u32 isr;

  // The routine establishing this IRQ handler MUST pass us the driver data in
  // the cookie.
  drvdata = drvdata_cookie;

  // Check to see if we even have an interrupt from this device
  if ((ioread32(drvdata->registers + HDMI_CTRL_OFF) & 0x200) == 0)
    return IRQ_NONE;
  // If we do, read the Interrupt Status Register to find out what interrupts
  // we need to service
  isr = ioread32(drvdata->registers + HDMI_ISR_OFF);
  BUG_ON(isr == 0);

  // At this point, we'd do whatever we need to do to service the interrupt,
  // which is fired on every frame. But, we don't do any double buffering, so we
  // don't need to do anything here. Just acknowledge all the interrupts so we
  // don't get called again, then return.
  iowrite32(isr, drvdata->registers + HDMI_ISR_OFF);
  return IRQ_HANDLED;
}

static int hdmi_probe_alloc_drvdata(struct platform_device *pdev) {
  // Helper function to allocate the driver data on the heap. It populates the
  // `drvdata` field in the `struct device` with the allocated data. All the
  // fields are zero-initialized.
  struct hdmi_driver_data *drvdata;

  drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
  if (!drvdata) {
    pr_err("failed to allocate driver data\n");
    return -ENOMEM;
  }

  pr_info("allocated driver data @ %p\n", drvdata);
  dev_set_drvdata(&pdev->dev, drvdata);
  return 0;
}

static int hdmi_probe_map_registers(struct platform_device *pdev) {
  // Helper function to map the device registers into our address space. It puts
  // the virtual address in the `registers` field of the driver data.
  struct hdmi_driver_data *drvdata;
  struct resource *res;
  void __iomem *reg;

  drvdata = dev_get_drvdata(&pdev->dev);
  res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
  reg = devm_ioremap_resource(&pdev->dev, res);
  if (IS_ERR(reg)) {
    pr_err("failed to map registers\n");
    return PTR_ERR(reg);
  }

  pr_info("mapped registers @ %p\n", reg);
  drvdata->registers = reg;
  return 0;
}

static int hdmi_probe_request_irq(struct platform_device *pdev) {
  // Helper function to request the IRQ for the device. It registers the
  // function `hdmi_irq_handler`, and passes it the driver data as the cookie.
  // Note that interrupts will not happen until the device is started.
  struct hdmi_driver_data *drvdata;
  int irq;
  int res;

  drvdata = dev_get_drvdata(&pdev->dev);

  irq = platform_get_irq(pdev, 0);
  if (irq < 0) {
    pr_err("failed to get IRQ\n");
    return irq;
  }

  res = devm_request_irq(&pdev->dev, irq, hdmi_irq_handler, 0,
                         "ammrat13-hdmi-dev", drvdata);
  if (res < 0) {
    pr_err("failed to request IRQ\n");
    return res;
  }

  pr_info("registered handler for IRQ %d\n", irq);
  return 0;
}

static int hdmi_probe_alloc_buffer(struct platform_device *pdev) {
  // Helper function to allocate the frame buffer in DMA memory. It puts both
  // the virtual and bus addresses of the buffer into the driver data.
  //
  // The buffer doesn't have to be physically contiguous in memory, as long as
  // its contiguous in bus memory. The kernel will use the IOMMU to ensure this,
  // or it will allocate it contiguously.
  //
  // This function also specifies the properties of the buffer. We allow write
  // coalescing via a store buffer.
  struct hdmi_driver_data *drvdata;
  void *vir;
  dma_addr_t bus;

  drvdata = dev_get_drvdata(&pdev->dev);

  vir = dmam_alloc_attrs(&pdev->dev, HDMI_BUF_LEN_BYTES, &bus, GFP_KERNEL,
                         DMA_ATTR_WRITE_COMBINE);
  if (!vir) {
    pr_err("failed to allocate buffer\n");
    return -ENOMEM;
  }

  pr_info("allocated buffer @ %p (bus: %x)\n", vir, bus);
  drvdata->buf_virt = vir;
  drvdata->buf_bus = bus;
  return 0;
}

static int hdmi_probe(struct platform_device *pdev) {
  struct hdmi_driver_data *drvdata;
  int res;

  pr_info("called probe on %p\n", pdev);

  // Call all the helper functions to initialize the platform device. All of
  // these should return 0 on success, and should not allocate any unmanaged
  // resources. These may depend on each other, so call them in the right order.
  if ((res = hdmi_probe_alloc_drvdata(pdev)) != 0)
    return res;
  if ((res = hdmi_probe_map_registers(pdev)) != 0)
    return res;
  if ((res = hdmi_probe_request_irq(pdev)) != 0)
    return res;
  if ((res = hdmi_probe_alloc_buffer(pdev)) != 0)
    return res;

  drvdata = dev_get_drvdata(&pdev->dev);
  hdmi_drvdata_assert_init(drvdata);

  // Tell the device the buffer address
  iowrite32(drvdata->buf_bus, drvdata->registers + HDMI_BUF_OFF);
  // Enable interrupts
  iowrite32(0x01, drvdata->registers + HDMI_GIE_OFF);
  iowrite32(0x03, drvdata->registers + HDMI_IER_OFF);
  // Start the device
  iowrite32(0x081, drvdata->registers + HDMI_CTRL_OFF);

  return 0;
}

static int hdmi_remove(struct platform_device *pdev) {
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
  struct hdmi_driver_data *drvdata;

  pr_info("called remove on %p\n", pdev);
  drvdata = dev_get_drvdata(&pdev->dev);
  hdmi_drvdata_assert_init(drvdata);

  // First and foremost, stop the device
  iowrite32(0x000, drvdata->registers + HDMI_CTRL_OFF);
  // Disable interrupts for the next guy
  iowrite32(0x00, drvdata->registers + HDMI_GIE_OFF);
  iowrite32(0x00, drvdata->registers + HDMI_IER_OFF);
  // Note that we keep the buffer address in the device. The next driver should
  // treat it as garbage, but it will allocate a new one.

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
