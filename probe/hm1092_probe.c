// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only presence test for the Himax HM1092 IR sensor on the Dell
 * Latitude 7455. It powers the sensor the way Windows does (CAMI_RES_QRD.bin:
 * power enable on GPIO 90, MCLK0 at 24 MHz, reset on GPIO 109), reads the
 * chip ID registers and powers it off again. It writes no sensor registers.
 *
 * The overlay puts one node on each camera I2C bus; every instance shares the
 * same GPIOs, so they are requested non-exclusively and used one at a time.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>

#define HM1092_MCLK_HZ	24000000

static DEFINE_MUTEX(hm1092_probe_lock);

static const char * const hm1092_supplies[] = { "avdd", "dvdd", "dovdd" };

static int hm1092_read(struct i2c_client *client, u16 reg, u8 *val, int len)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0, .len = 2, .buf = addr },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = len, .buf = val },
	};
	int ret = i2c_transfer(client->adapter, msgs, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int hm1092_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset;
	struct clk *mclk;
	u8 id[4] = { 0 }, b;
	int i, ret;

	/*
	 * devm keeps a pointer to this array until the device's resources are
	 * released (after probe returns), so it must not live on the stack.
	 */
	supplies = devm_kcalloc(dev, ARRAY_SIZE(hm1092_supplies),
				sizeof(*supplies), GFP_KERNEL);
	if (!supplies)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(hm1092_supplies); i++)
		supplies[i].supply = hm1092_supplies[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(hm1092_supplies), supplies);
	if (ret)
		return dev_err_probe(dev, ret, "supplies\n");

	mclk = devm_clk_get(dev, NULL);
	if (IS_ERR(mclk))
		return dev_err_probe(dev, PTR_ERR(mclk), "mclk\n");

	/* Starts asserted: logical 1 is "in reset" (the line is active low). */
	reset = devm_gpiod_get(dev, "reset",
			       GPIOD_OUT_HIGH | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "reset gpio\n");

	mutex_lock(&hm1092_probe_lock);

	ret = regulator_bulk_enable(ARRAY_SIZE(hm1092_supplies), supplies);
	if (ret) {
		dev_err(dev, "enabling supplies failed: %d\n", ret);
		goto unlock;
	}
	usleep_range(1000, 2000);

	ret = clk_set_rate(mclk, HM1092_MCLK_HZ);
	if (!ret)
		ret = clk_prepare_enable(mclk);
	if (ret) {
		dev_err(dev, "mclk at %u Hz failed: %d\n", HM1092_MCLK_HZ, ret);
		goto power_off;
	}
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(reset, 0);
	msleep(10);

	ret = hm1092_read(client, 0x0000, id, 4);
	if (ret)
		dev_info(dev, "no answer at 0x%02x on %s: %d\n", client->addr,
			 dev_name(&client->adapter->dev), ret);
	else
		dev_info(dev, "ANSWERED at 0x%02x on %s: regs 0x0000-0x0003 = %02x %02x %02x %02x\n",
			 client->addr, dev_name(&client->adapter->dev),
			 id[0], id[1], id[2], id[3]);
	/* One byte per read too, in case the sensor does not auto-increment. */
	if (!ret)
		for (i = 0; i < 4; i++)
			if (!hm1092_read(client, i, &b, 1))
				dev_info(dev, "  reg 0x%04x = %02x\n", i, b);

	gpiod_set_value_cansleep(reset, 1);
	clk_disable_unprepare(mclk);
power_off:
	regulator_bulk_disable(ARRAY_SIZE(hm1092_supplies), supplies);
unlock:
	mutex_unlock(&hm1092_probe_lock);

	/* Stay unbound either way; this module only reports. */
	return -ENODEV;
}

static const struct of_device_id hm1092_probe_of_match[] = {
	{ .compatible = "himax,hm1092-probe" },
	{ }
};
MODULE_DEVICE_TABLE(of, hm1092_probe_of_match);

static struct i2c_driver hm1092_probe_driver = {
	.driver = {
		.name = "hm1092-probe",
		.of_match_table = hm1092_probe_of_match,
	},
	.probe = hm1092_probe,
};
module_i2c_driver(hm1092_probe_driver);

MODULE_DESCRIPTION("HM1092 IR sensor presence test (read-only)");
MODULE_LICENSE("GPL");
