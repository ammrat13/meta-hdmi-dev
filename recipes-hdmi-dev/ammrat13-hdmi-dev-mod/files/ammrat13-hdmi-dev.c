#define pr_fmt(fmt) "ammrat13-hdmi-dev: " fmt

#include <linux/module.h>

#include <linux/device/driver.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

int ammrat13_hdmi_probe(struct platform_device *pdev) {
  pr_info("called probe on %p\n", pdev);
  return 0;
}

int ammrat13_hdmi_remove(struct platform_device *pdev) {
  // The mainline kernel says that the return value from this function is
  // ignored. So, we always return 0. It also means we have to do our own error
  // handling if needed.

  pr_info("called remove on %p\n", pdev);
  return 0;
}

// Match table for which devices in the device tree to probe with this driver.
// This is included in the `struct platform_driver` below. The names for the
// `compatible` field are taken from the final device tree itself.
static struct of_device_id ammrat13_hdmi_match_table[] = {
  { .compatible = "xlnx,hdmi-cmd-gen-0.0" },
  { .compatible = "xlnx,hdmi-cmd-gen" },
  { /* null-terminator */ },
};
// Structure describing this driver. This will be passed to the kernel for
// matching on the device tree.
static struct platform_driver ammrat13_hdmi_driver = {
  .probe = ammrat13_hdmi_probe,
  .remove = ammrat13_hdmi_remove,
  .driver = {
    .name = "ammrat13-hdmi-dev",
    .owner = THIS_MODULE,
    .of_match_table = ammrat13_hdmi_match_table,
  },
};
// This macro defines the `init` and `exit` functions for the module. They just
// forward to the `probe` and `remove` functions of the driver.
module_platform_driver(ammrat13_hdmi_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ammar Ratnani <ammrat13@gmail.com>");
MODULE_DESCRIPTION("Kernel module to drive ammrat13's HDMI Peripheral");
