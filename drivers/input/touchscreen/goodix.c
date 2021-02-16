/*
 *  Driver for Goodix Touchscreens
 *
 *  Copyright (c) 2014 Red Hat Inc.
 *
 *  This code is based on gt9xx.c authored by andrew@goodix.com:
 *
 *  2010 - 2012 Goodix Technology.
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 2 of the License.
 */

#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include <asm/unaligned.h>

#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/input/mt.h>

#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/export.h>

#define	GOODIX_DEBUG
#define GOODIX_MAX_HEIGHT		480//4096
#define GOODIX_MAX_WIDTH		800//4096
#define GOODIX_INT_TRIGGER		1
#define GOODIX_CONTACT_SIZE		(8)
#define GOODIX_MAX_CONTACTS		5

#define GOODIX_CONFIG_MAX_LENGTH	240

/* Register defines */
#define GOODIX_READ_COOR_ADDR		0x814E
#define GOODIX_REG_CONFIG_DATA		0x8047
#define GOODIX_REG_VERSION		0x8140

#define RESOLUTION_LOC		1
#define MAX_CONTACTS_LOC	5
#define TRIGGER_LOC		6

#define USE_WORK_NOT		0
#define USE_WORK_DELAY		1
#define USE_WORK_NORAMAL 	2
#define USE_WORK	USE_WORK_DELAY

struct goodix_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	int abs_x_max;
	int abs_y_max;
	unsigned int max_touch_num;
	unsigned int int_trigger_type;
#if (USE_WORK == USE_WORK_NORAMAL)	
	struct work_struct work;
#elif (USE_WORK == USE_WORK_DELAY)
	struct delayed_work work;
#endif	
};

static const unsigned long goodix_irq_flags[] = {
	IRQ_TYPE_EDGE_RISING,
	IRQ_TYPE_EDGE_FALLING,
	IRQ_TYPE_LEVEL_LOW,
	IRQ_TYPE_LEVEL_HIGH,
};
static int ts_reset_device(struct i2c_client *client);
/**
 * goodix_i2c_read - read data from a register of the i2c slave device.
 *
 * @client: i2c device.
 * @reg: the register to read from.
 * @buf: raw write data buffer.
 * @len: length of the buffer to write
 */
static int goodix_i2c_read(struct i2c_client *client,
				u16 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[2];
	u16 wbuf = cpu_to_be16(reg);
	int ret;

	msgs[0].flags = 0;
	msgs[0].addr  = client->addr;
	msgs[0].len   = 2;
	msgs[0].buf   = (u8 *) &wbuf;

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr  = client->addr;
	msgs[1].len   = len;
	msgs[1].buf   = buf;

	ret = i2c_transfer(client->adapter, msgs, 2);
	return ret < 0 ? ret : (ret != ARRAY_SIZE(msgs) ? -EIO : 0);
}

static int goodix_ts_read_input_report(struct goodix_ts_data *ts, u8 *data)
{
	int touch_num;
	int error;
	int finger;

	error = goodix_i2c_read(ts->client, GOODIX_READ_COOR_ADDR, data,
				GOODIX_CONTACT_SIZE + 1);
	if (error) {
		dev_err(&ts->client->dev, "I2C transfer error: %d\n", error);
		return error;
	}
	finger = data[0];

	if(finger == 0x00) {
		return -EPROTO;
	}
	
	if((finger & 0x80) == 0) {
		return -EPROTO;
	}
	touch_num = finger & 0x0f;

	dev_dbg(&ts->client->dev, "read data len finger=%d\n", touch_num);

	if (touch_num > ts->max_touch_num)
		return -EPROTO;

	if (touch_num > 1) {
		data += 1 + GOODIX_CONTACT_SIZE;
		error = goodix_i2c_read(ts->client,
					GOODIX_READ_COOR_ADDR +
						1 + GOODIX_CONTACT_SIZE,
					data,
					GOODIX_CONTACT_SIZE * (touch_num - 1));
		if (error)
			return error;
	}

	return touch_num;
}

static void goodix_ts_report_touch(struct goodix_ts_data *ts, u8 *coor_data)
{
	int id = coor_data[1] & 0x0F;
	int input_x = get_unaligned_le16(&coor_data[2]);
	int input_y = get_unaligned_le16(&coor_data[4]);
	int input_w = get_unaligned_le16(&coor_data[6]);
	
#if 0
	dev_dbg(&ts->client->dev, "x=%d,y=%d,w=%d\n",input_x,input_y,input_w);
	input_mt_slot(ts->input_dev, id);
	input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, input_x);
	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, input_y);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, input_w);
	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, input_w);
#else
	//printk("x=%d,y=%d,w=%d\n",input_y,input_x,input_w);
	input_report_key(ts->input_dev, BTN_TOUCH, 1);
	input_report_abs(ts->input_dev, ABS_X, input_y);
        input_report_abs(ts->input_dev, ABS_Y, input_x);
        input_report_abs(ts->input_dev, ABS_PRESSURE, 200);

	input_sync(ts->input_dev);
#endif
}

/**
 * goodix_process_events - Process incoming events
 *
 * @ts: our goodix_ts_data pointer
 *
 * Called when the IRQ is triggered. Read the current device state, and push
 * the input events to the user space.
 */
static void goodix_process_events(struct goodix_ts_data *ts)
{
	u8  point_data[1 + GOODIX_CONTACT_SIZE * ts->max_touch_num];
	int touch_num;
	int i;
	
	touch_num = goodix_ts_read_input_report(ts, point_data);
	//if(touch_num) {
	if(touch_num == 1) {
		//for (i = 0; i < touch_num; i++)
		for (i = 0; i < 1; i++)
			goodix_ts_report_touch(ts,
					&point_data[GOODIX_CONTACT_SIZE * i]);

		//input_report_key(ts->input_dev, BTN_TOUCH, 1);
		//input_sync(ts->input_dev);
	}
	else
	{
		input_report_key(ts->input_dev, BTN_TOUCH, 0);
        	input_report_abs(ts->input_dev, ABS_PRESSURE, 0);
        	input_sync(ts->input_dev);
	}

	//input_mt_sync_frame(ts->input_dev);
	//input_sync(ts->input_dev);
}

static void goodix_work(struct work_struct *pwork)
{
	struct goodix_ts_data *ts = container_of(pwork, struct goodix_ts_data, work);
	static const u8 end_cmd[] = {
		GOODIX_READ_COOR_ADDR >> 8,
		GOODIX_READ_COOR_ADDR & 0xff,
		0
	};	
	goodix_process_events(ts);

	if (i2c_master_send(ts->client, end_cmd, sizeof(end_cmd)) < 0)
		dev_err(&ts->client->dev, "I2C write end_cmd error\n");
	enable_irq(ts->client->irq);
}



/**
 * goodix_ts_irq_handler - The IRQ handler
 *
 * @irq: interrupt number.
 * @dev_id: private data pointer.
 */
static irqreturn_t goodix_ts_irq_handler(int irq, void *dev_id)
{
	static const u8 end_cmd[] = {
		GOODIX_READ_COOR_ADDR >> 8,
		GOODIX_READ_COOR_ADDR & 0xff,
		0
	};
	struct goodix_ts_data *ts = dev_id;
	disable_irq_nosync(irq);
#if (USE_WORK == USE_WORK_NOT)
	goodix_process_events(ts);

	if (i2c_master_send(ts->client, end_cmd, sizeof(end_cmd)) < 0)
		dev_err(&ts->client->dev, "I2C write end_cmd error\n");
#elif (USE_WORK == USE_WORK_NORAMAL)
	dev_dbg(&ts->client->dev, "schedule_work\n");
	schedule_work(&ts->work);
#elif (USE_WORK == USE_WORK_DELAY)
	schedule_delayed_work(&ts->work, msecs_to_jiffies(15));
#endif

	return IRQ_HANDLED;
}

/**
 * goodix_read_config - Read the embedded configuration of the panel
 *
 * @ts: our goodix_ts_data pointer
 *
 * Must be called during probe
 */
static void goodix_read_config(struct goodix_ts_data *ts)
{
	u8 config[GOODIX_CONFIG_MAX_LENGTH];
	int error;

	error = goodix_i2c_read(ts->client, GOODIX_REG_CONFIG_DATA,
			      config,
			   GOODIX_CONFIG_MAX_LENGTH);
	if (error) {
		dev_warn(&ts->client->dev,
			 "Error reading config (%d), using defaults\n",
			 error);
		ts->abs_x_max = GOODIX_MAX_WIDTH;
		ts->abs_y_max = GOODIX_MAX_HEIGHT;
		ts->int_trigger_type = GOODIX_INT_TRIGGER;
		ts->max_touch_num = GOODIX_MAX_CONTACTS;
		return;
	}

	ts->abs_x_max = get_unaligned_le16(&config[RESOLUTION_LOC]);
	ts->abs_y_max = get_unaligned_le16(&config[RESOLUTION_LOC + 2]);
	ts->int_trigger_type = config[TRIGGER_LOC] & 0x03;
	ts->max_touch_num = config[MAX_CONTACTS_LOC] & 0x0f;
	if (!ts->abs_x_max || !ts->abs_y_max || !ts->max_touch_num) {
		dev_err(&ts->client->dev,
			"Invalid config, using defaults\n");
		ts->abs_x_max = GOODIX_MAX_WIDTH;
		ts->abs_y_max = GOODIX_MAX_HEIGHT;
		ts->max_touch_num = GOODIX_MAX_CONTACTS;
	}
	ts->max_touch_num = GOODIX_MAX_CONTACTS;
}

/**
 * goodix_read_version - Read goodix touchscreen version
 *
 * @client: the i2c client
 * @version: output buffer containing the version on success
 */
static int goodix_read_version(struct i2c_client *client, u16 *version)
{
	int error;
	u8 buf[6];

	error = goodix_i2c_read(client, GOODIX_REG_VERSION, buf, sizeof(buf));
	if (error) {
		dev_err(&client->dev, "read version failed: %d\n", error);
		return error;
	}

	if (version)
		*version = get_unaligned_le16(&buf[4]);

	dev_info(&client->dev, "IC VERSION: %6ph\n", buf);

	return 0;
}

/**
 * goodix_i2c_test - I2C test function to check if the device answers.
 *
 * @client: the i2c client
 */
static int goodix_i2c_test(struct i2c_client *client)
{
	int retry = 0;
	int error;
	u8 test;

	while (retry++ < 2) {
		error = goodix_i2c_read(client, GOODIX_REG_CONFIG_DATA,
					&test, 1);
		if (!error)
			return 0;

		dev_err(&client->dev, "i2c test failed attempt %d: %d\n",\
			retry, error);
		msleep(20);
	}

	return error;
}

/**
 * goodix_request_input_dev - Allocate, populate and register the input device
 *
 * @ts: our goodix_ts_data pointer
 *
 * Must be called during probe
 */
static int goodix_request_input_dev(struct goodix_ts_data *ts)
{
	int error;

	ts->input_dev = devm_input_allocate_device(&ts->client->dev);
	if (!ts->input_dev) {
		dev_err(&ts->client->dev, "Failed to allocate input device.");
		return -ENOMEM;
	}
#if 0
	ts->input_dev->evbit[0] = BIT_MASK(EV_SYN) |
				  BIT_MASK(EV_KEY) |
				  BIT_MASK(EV_ABS);
	ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0,
				ts->abs_x_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0,
				ts->abs_y_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	input_mt_init_slots(ts->input_dev, ts->max_touch_num,
			    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
#else
	set_bit(EV_SYN, ts->input_dev->evbit);
        set_bit(EV_ABS, ts->input_dev->evbit);
        set_bit(EV_KEY, ts->input_dev->evbit);

        set_bit(ABS_X, ts->input_dev->absbit);
        set_bit(ABS_Y, ts->input_dev->absbit);
        set_bit(ABS_PRESSURE, ts->input_dev->absbit);
        set_bit(BTN_TOUCH, ts->input_dev->keybit);

        input_set_abs_params(ts->input_dev, ABS_X, 0, 800, 0, 0);
        input_set_abs_params(ts->input_dev, ABS_Y, 0, 1280, 0, 0);
        input_set_abs_params(ts->input_dev, ABS_PRESSURE, 0, 200, 0, 0);
#endif
	ts->input_dev->name = "goodix-gt911";//"Capacitance_ts";//"Goodix Capacitive TouchScreen";
	ts->input_dev->phys = "input/ts";
	ts->input_dev->id.bustype = BUS_I2C;
	ts->input_dev->id.vendor = 0x0416;
	ts->input_dev->id.product = 0x1001;
	ts->input_dev->id.version = 10427;

	error = input_register_device(ts->input_dev);
	if (error) {
		dev_err(&ts->client->dev,
			"Failed to register input device: %d", error);
		return error;
	}

	return 0;
}



/* wake up controller by an falling edge of interrupt gpio.  */
static int ts_reset_device(struct i2c_client *client)
{
	struct device_node *np = client->dev.of_node;
	int gpio_rst;
	int gpio_int;
	int ret,i;

	if (!np)
		return -ENODEV;

	gpio_rst = of_get_named_gpio(np, "goodix_rst", 0);
	if (!gpio_is_valid(gpio_rst))
		return -ENODEV;
	
	ret = gpio_request(gpio_rst, "goodix_rst");
	if (ret < 0) {
		dev_err(&client->dev,
			"request gpio failed, cannot wake up controller: %d\n",
			ret);
		return ret;
	}
	gpio_int = of_get_named_gpio(np, "goodix_int", 0);
	if (!gpio_is_valid(gpio_int))
		return -ENODEV;	
	ret = gpio_request(gpio_int, "goodix_int");
	if (ret < 0) {
		dev_err(&client->dev,
			"request gpio_int failed, cannot wake up controller: %d\n",
			ret);
		return ret;
	}
#if 0
	log_msg(LOG_DEBUG,"set io to rst\n");
	for(i=0;i<500;++i)
	{
		gpio_direction_output(gpio_rst,0);
		msleep(10);
		gpio_direction_output(gpio_rst,1);
		msleep(10);
	}
	log_msg(LOG_DEBUG,"set io to int\n");
	for(i=0;i<500;++i)
	{
		gpio_direction_output(gpio_int,0);
		msleep(10);
		gpio_direction_output(gpio_int,1);
		msleep(10);
	}
#endif	

	/* wake up controller via an falling edge on IRQ gpio. */
	gpio_direction_output(gpio_rst, 0);
	mdelay(20);
	gpio_direction_output(gpio_int, 0);
	mdelay(2);
	gpio_set_value(gpio_rst, 1);
	/* controller should be waken up, return irq.  */
	mdelay(6);
	gpio_direction_input(gpio_rst);
	
	gpio_direction_output(gpio_int, 1);
	mdelay(50);
	gpio_direction_input(gpio_int);
	//client->irq = gpio_to_irq(gpio_int);
	

	gpio_free(gpio_rst);
	//gpio_free(gpio_int);

	return 0;
}
char g_ts_name[32]={'\0'};

static int goodix_ts_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct goodix_ts_data *ts;
	unsigned long irq_flags;
	int error;
	u16 version_info;

	dev_dbg(&client->dev, "I2C Address: 0x%02x\n", client->addr);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C check functionality failed.\n");
		return -ENXIO;
	}

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);

	error = ts_reset_device(client);
	if(error < 0) {
		return error;
	}
	
	error = goodix_i2c_test(client);
	if (error) {
		dev_err(&client->dev, "I2C communication failure: %d\n", error);
		return error;
	}

	error = goodix_read_version(client, &version_info);
	if (error) {
		dev_err(&client->dev, "Read version failed.\n");
		return error;
	}

	goodix_read_config(ts);

	error = goodix_request_input_dev(ts);
	if (error)
		return error;

	irq_flags = goodix_irq_flags[ts->int_trigger_type] | IRQF_ONESHOT;
	dev_dbg(&ts->client->dev, "irq(%d) triggered=%d,flag=0x%x\n",client->irq,ts->int_trigger_type,irq_flags);
#if (USE_WORK == USE_WORK_NOT)	
	irq_flags = IRQ_TYPE_EDGE_FALLING | IRQF_ONESHOT;
	error = devm_request_threaded_irq(&ts->client->dev, client->irq, \
					  NULL, goodix_ts_irq_handler, \
					  irq_flags, client->name, ts);
#elif (USE_WORK == USE_WORK_NORAMAL)
	irq_flags = IRQ_TYPE_EDGE_FALLING | IRQF_ONESHOT;
	INIT_WORK(&ts->work, goodix_work);
	error = request_any_context_irq(client->irq, goodix_ts_irq_handler,\
						 irq_flags, client->name,ts);
	//error = devm_request_irq(&ts->client->dev, client->irq, \
					  goodix_ts_irq_handler, irq_flags, client->name, ts);
#elif (USE_WORK == USE_WORK_DELAY)
	irq_flags = IRQ_TYPE_LEVEL_LOW | IRQF_ONESHOT;
	INIT_DELAYED_WORK(&ts->work, goodix_work);			
	error = request_any_context_irq(client->irq, goodix_ts_irq_handler,\
						 irq_flags, client->name,ts);
#endif
	if (error) {
		dev_err(&client->dev, "request IRQ failed: %d\n", error);
		return error;
	}
	dev_dbg(&ts->client->dev, "exit success\n");
	strcpy(g_ts_name,ts->input_dev->name);

	return 0;
}

static const struct i2c_device_id goodix_ts_id[] = {
	{ "GDIX1001:00", 0 },
	{ }
};

#ifdef CONFIG_ACPI
static const struct acpi_device_id goodix_acpi_match[] = {
	{ "GDIX1001", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, goodix_acpi_match);
#endif

#ifdef CONFIG_OF
static const struct of_device_id goodix_of_match[] = {
	{ .compatible = "goodix,gt911" },
	{ .compatible = "goodix,gt9110" },
	{ .compatible = "goodix,gt912" },
	{ .compatible = "goodix,gt927" },
	{ .compatible = "goodix,gt9271" },
	{ .compatible = "goodix,gt928" },
	{ .compatible = "goodix,gt967" },
	{ }
};
MODULE_DEVICE_TABLE(of, goodix_of_match);
#endif

static struct i2c_driver goodix_ts_driver = {
	.probe = goodix_ts_probe,
	.id_table = goodix_ts_id,
	.driver = {
		.name = "Goodix-TS",
		.owner = THIS_MODULE,
		.acpi_match_table = ACPI_PTR(goodix_acpi_match),
		.of_match_table = of_match_ptr(goodix_of_match),
	},
};
module_i2c_driver(goodix_ts_driver);

MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_AUTHOR("Bastien Nocera <hadess@hadess.net>");
MODULE_DESCRIPTION("Goodix touchscreen driver");
MODULE_LICENSE("GPL v2");