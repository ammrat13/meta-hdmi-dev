#define pr_fmt(fmt) "ammrat13-hdmi-dev: " fmt

#include <linux/module.h>

#include <asm/io.h>
#include <linux/device/driver.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

// Structure describing an HDMI Peripheral. We allocate one of these on probe,
// and we stick it in the `driver_data` field of the device.
struct hdmi_driver_data {

  // The MMIO region for the device, mapped into our virtual address space
  void __iomem *registers;
};

// Byte offsets for all the registers
#define HDMI_CTRL_OFF 0x00
#define HDMI_GIE_OFF 0x04
#define HDMI_IER_OFF 0x08
#define HDMI_ISR_OFF 0x0c
#define HDMI_FRAMEBUF_OFF 0x10
#define HDMI_COORD_DATA_OFF 0x18
#define HDMI_COORD_CTRL_OFF 0x1c

int hdmi_probe(struct platform_device *pdev) {

  // Pointer to this device's driver data on the heap
  struct hdmi_driver_data *ddata;

  // Log
  pr_info("called probe on %p\n", pdev);

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

  // Start the device
  iowrite32(0x081, ddata->registers + HDMI_CTRL_OFF);

  return 0;
}

int hdmi_remove(struct platform_device *pdev) {
  // When we were probing this device, we did our best to use managed resources.
  // This means they will be cleaned up automatically when this function
  // returns. We just have to deal with the non-managed resources.
  //
  // The mainline kernel says that the return value from this function is
  // ignored. So, we always return 0. It also means we have to do our own error
  // handling if needed.

  // Pointer to this device's driver data on the heap
  struct hdmi_driver_data *ddata;

  // Log and initialize variables
  pr_info("called remove on %p\n", pdev);
  ddata = dev_get_drvdata(&pdev->dev);

  // Stop the device
  iowrite32(0x000, ddata->registers + HDMI_CTRL_OFF);

  return 0;
}

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
