// SPDX-License-Identifier: GPL-2.0

#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

static int s6sy761_power_on(struct s6sy761_data *sdata)
{
	u8 buffer[S6SY761_EVENT_SIZE];
	u8 event;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(sdata->regulators),
				    sdata->regulators);
	if (ret)
		return ret;

	msleep(140);

	/* double check whether the touch is functional */
	ret = i2c_smbus_read_i2c_block_data(sdata->client,
					    S6SY761_READ_ONE_EVENT,
					    S6SY761_EVENT_SIZE,
					    buffer);
	if (ret < 0)
		return ret;

	event = (buffer[0] >> 2) & 0xf;

	if ((event != S6SY761_EVENT_INFO &&
	     event != S6SY761_EVENT_VENDOR_INFO) ||
	    buffer[1] != S6SY761_INFO_BOOT_COMPLETE) {
		return -ENODEV;
	}

	ret = i2c_smbus_read_byte_data(sdata->client, S6SY761_BOOT_STATUS);
	if (ret < 0)
		return ret;

	/* for some reasons the device might be stuck in the bootloader */
	if (ret != S6SY761_BS_APPLICATION)
		return -ENODEV;

	/* enable touch functionality */
	ret = i2c_smbus_write_word_data(sdata->client,
					S6SY761_TOUCH_FUNCTION,
					S6SY761_MASK_TOUCH);
	if (ret)
		return ret;

	return 0;
}

static int s6sy761_hw_init(struct s6sy761_data *sdata,
			   unsigned int *max_x, unsigned int *max_y)
{
	u8 buffer[S6SY761_PANEL_ID_SIZE]; /* larger read size */
	int ret;

	ret = s6sy761_power_on(sdata);
	if (ret)
		return ret;

	ret = i2c_smbus_read_i2c_block_data(sdata->client,
					    S6SY761_DEVICE_ID,
					    S6SY761_DEVID_SIZE,
					    buffer);
	if (ret < 0)
		return ret;

	sdata->devid = get_unaligned_be16(buffer + 1);

	ret = i2c_smbus_read_i2c_block_data(sdata->client,
					    S6SY761_PANEL_INFO,
					    S6SY761_PANEL_ID_SIZE,
					    buffer);
	if (ret < 0)
		return ret;

	*max_x = get_unaligned_be16(buffer);
	*max_y = get_unaligned_be16(buffer + 2);

	/* if no tx channels defined, at least keep one */
	sdata->tx_channel = max_t(u8, buffer[8], 1);

	ret = i2c_smbus_read_byte_data(sdata->client,
				       S6SY761_FIRMWARE_INTEGRITY);
	if (ret < 0)
		return ret;
	else if (ret != S6SY761_FW_OK)
		return -ENODEV;

	return 0;
}

static void s6sy761_power_off(void *data)
{
	struct s6sy761_data *sdata = data;

	disable_irq(sdata->client->irq);
	regulator_bulk_disable(ARRAY_SIZE(sdata->regulators),
						sdata->regulators);
}

static int s6sy761_probe(struct i2c_client *client)
{
	struct s6sy761_data *sdata;
	unsigned int max_x, max_y;
	int err;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C |
						I2C_FUNC_SMBUS_BYTE_DATA |
						I2C_FUNC_SMBUS_I2C_BLOCK))
		return -ENODEV;

	sdata = devm_kzalloc(&client->dev, sizeof(*sdata), GFP_KERNEL);
	if (!sdata)
		return -ENOMEM;

	i2c_set_clientdata(client, sdata);
	sdata->client = client;

	sdata->regulators[S6SY761_REGULATOR_VDD].supply = "vdd";
	sdata->regulators[S6SY761_REGULATOR_AVDD].supply = "avdd";
	err = devm_regulator_bulk_get(&client->dev,
				      ARRAY_SIZE(sdata->regulators),
				      sdata->regulators);
	if (err)
		return err;

	err = devm_add_action_or_reset(&client->dev, s6sy761_power_off, sdata);
	if (err)
		return err;

	err = s6sy761_hw_init(sdata, &max_x, &max_y);
	if (err)
		return err;

	sdata->input = devm_input_allocate_device(&client->dev);
	if (!sdata->input)
		return -ENOMEM;

	sdata->input->name = S6SY761_DEV_NAME;
	sdata->input->id.bustype = BUS_I2C;
	sdata->input->open = s6sy761_input_open;
	sdata->input->close = s6sy761_input_close;

	input_set_abs_params(sdata->input, ABS_MT_POSITION_X, 0, max_x, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_POSITION_Y, 0, max_y, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(sdata->input, ABS_MT_PRESSURE, 0, 255, 0, 0);

	touchscreen_parse_properties(sdata->input, true, &sdata->prop);

	if (!input_abs_get_max(sdata->input, ABS_X) ||
	    !input_abs_get_max(sdata->input, ABS_Y)) {
		dev_warn(&client->dev, "the axis have not been set\n");
	}

	err = input_mt_init_slots(sdata->input, sdata->tx_channel,
				  INPUT_MT_DIRECT);
	if (err)
		return err;

	input_set_drvdata(sdata->input, sdata);

	err = input_register_device(sdata->input);
	if (err)
		return err;

	err = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					s6sy761_irq_handler,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					"s6sy761_irq", sdata);
	if (err)
		return err;

	pm_runtime_enable(&client->dev);

	return 0;
}

static void s6sy761_remove(struct i2c_client *client)
{
	pm_runtime_disable(&client->dev);
}

static int s6sy761_runtime_suspend(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);

	return i2c_smbus_write_byte_data(sdata->client,
				S6SY761_APPLICATION_MODE, S6SY761_APP_SLEEP);
}

static int s6sy761_runtime_resume(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);

	return i2c_smbus_write_byte_data(sdata->client,
				S6SY761_APPLICATION_MODE, S6SY761_APP_NORMAL);
}

static int s6sy761_suspend(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);

	s6sy761_power_off(sdata);

	return 0;
}

static int s6sy761_resume(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);

	enable_irq(sdata->client->irq);

	return s6sy761_power_on(sdata);
}

static const struct dev_pm_ops s6sy761_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(s6sy761_suspend, s6sy761_resume)
	RUNTIME_PM_OPS(s6sy761_runtime_suspend, s6sy761_runtime_resume, NULL)
};

#ifdef CONFIG_OF
static const struct of_device_id s6sy761_of_match[] = {
	{ .compatible = "samsung,s6sy761", },
	{ },
};
MODULE_DEVICE_TABLE(of, s6sy761_of_match);
#endif

static const struct i2c_device_id s6sy761_id[] = {
	{ "s6sy761", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, s6sy761_id);

static struct i2c_driver s6sy761_driver = {
	.driver = {
		.name = S6SY761_DEV_NAME,
		.dev_groups = s6sy761_sysfs_groups,
		.of_match_table = of_match_ptr(s6sy761_of_match),
		.pm = pm_ptr(&s6sy761_pm_ops),
	},
	.probe = s6sy761_probe,
	.remove = s6sy761_remove,
	.id_table = s6sy761_id,
};

module_i2c_driver(s6sy761_driver);

MODULE_AUTHOR("Andi Shyti <andi.shyti@samsung.com>");
MODULE_DESCRIPTION("Samsung S6SY761 Touch Screen");
MODULE_LICENSE("GPL v2");
