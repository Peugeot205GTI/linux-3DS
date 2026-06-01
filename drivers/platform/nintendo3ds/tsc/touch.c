// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tsc/touch.c
 *
 * Copyright (C) 2016 Sergi Granell
 * Copyright (C) 2017 Paul LaMendola
 * Copyright (C) 2020-2021 Santiago Herrera
 * based on ad7879-spi.c
 */

#define DRIVER_NAME	"3dstsc-touch"
#define pr_fmt(fmt)	DRIVER_NAME ": " fmt

#include <linux/of.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>

#include <linux/font.h>
#include <asm/io.h>

#define POLL_INTERVAL_DEFAULT		16 /* ~60fps */
#define MAX_12BIT			((1 << 12) - 1)
#define CIRCLE_PAD_THRESHOLD		150
#define CIRCLE_PAD_FACTOR		150

#define LEFT_SHIFTED  BIT(0)
#define RIGHT_SHIFTED BIT(1)

#define TOUCH_REG(reg)	((0x67 << 7) | (reg)) /* bank 67h, register xxh */
#define TOUCH_FIFO_REG	((0xFB << 7) | 0x01) /* bank FBh, register 01h */


struct touch_hid {
	struct device *dev;
	struct regmap *map;
	struct input_dev *input_dev;

	unsigned long touch_jiffies;
	bool pendown;
};

struct touch_fifo_data {
	u16 touch[2][5];
	u16 cpad[2][8];
} __packed;



static int touch_initialize(struct regmap *map)
{
	/* magic init sequence */
	static const struct reg_sequence initseq[] = {
		REG_SEQ(TOUCH_REG(0x24), 0x98, 10),
		REG_SEQ(TOUCH_REG(0x26), 0x00, 10),
		REG_SEQ(TOUCH_REG(0x25), 0x43, 10),
		REG_SEQ(TOUCH_REG(0x24), 0x18, 10),
		REG_SEQ(TOUCH_REG(0x17), 0x43, 10),
		REG_SEQ(TOUCH_REG(0x19), 0x69, 10),
		REG_SEQ(TOUCH_REG(0x1B), 0x80, 10),
		REG_SEQ(TOUCH_REG(0x27), 0x11, 10),
		REG_SEQ(TOUCH_REG(0x26), 0xEC, 10),
		REG_SEQ(TOUCH_REG(0x24), 0x18, 10),
		REG_SEQ(TOUCH_REG(0x25), 0x53, 10)
	};

	return regmap_multi_reg_write(map, initseq, ARRAY_SIZE(initseq));
}

static int touch_enable(struct regmap *map)
{
	int err = regmap_update_bits(map, TOUCH_REG(0x26), 0x80, 0x80);
	if (err) return err;

	err = regmap_update_bits(map, TOUCH_REG(0x24), 0x80, 0x00);
	if (err) return err;

	return regmap_update_bits(map, TOUCH_REG(0x25), 0x3C, 0x10);
}

static int touch_disable(struct regmap *map)
{
	int err = regmap_update_bits(map, TOUCH_REG(0x26), 0x80, 0x00);
	if (err) return err;

	return regmap_update_bits(map, TOUCH_REG(0x24), 0x80, 0x80);
}

static int touch_request_data(struct regmap *map, u8 *buffer)
{
	int err;
	unsigned int reg;

	/* acknowledge touch? */
	err = regmap_read(map, TOUCH_REG(0x26), &reg);
	if (err) return err;

	/* no new data available */
	if (reg & BIT(1))
		return -ENODATA;

	return regmap_bulk_read(map, TOUCH_FIFO_REG, buffer, 0x34);
}

static void touch_input_poll(struct input_dev *input)
{
	struct touch_hid *touch_hid = input_get_drvdata(input);
	struct vkb_ctx_t *vkb = &touch_hid->vkb;

	u8 raw_data[0x40] __attribute__((aligned(sizeof(u32))));
	bool pendown;
	u16 raw_touch_x;
	u16 raw_touch_y;
	u16 screen_touch_x;
	u16 screen_touch_y;
	s16 raw_circlepad_x;
	s16 raw_circlepad_y;
	bool sync = false;
	int i, j, err;

	err = touch_request_data(touch_hid->map, raw_data);
	if (err == -ENODATA)
		return;

	if (err) {
		/*
			something bad happened
			TODO: reboot the controller or something?
		*/
		return;
	}

	raw_circlepad_x =
		(s16)le16_to_cpu(((raw_data[0x24] << 8) | raw_data[0x25]) & 0xFFF) - 2048;
	raw_circlepad_y =
		(s16)le16_to_cpu(((raw_data[0x14] << 8) | raw_data[0x15]) & 0xFFF) - 2048;

	if (abs(raw_circlepad_x) > CIRCLE_PAD_THRESHOLD) {
		input_report_rel(input, REL_X,
				 -raw_circlepad_x / CIRCLE_PAD_FACTOR);
		sync = true;
	}

	if (abs(raw_circlepad_y) > CIRCLE_PAD_THRESHOLD) {
		input_report_rel(input, REL_Y,
				 -raw_circlepad_y / CIRCLE_PAD_FACTOR);
		sync = true;
	}

	pendown = !(raw_data[0] & BIT(4));

	if (pendown) {
		if(!touch_hid->pendown) {
			raw_touch_x = le16_to_cpu((raw_data[0]  << 8) | raw_data[1]);
			raw_touch_y = le16_to_cpu((raw_data[10] << 8) | raw_data[11]);

			screen_touch_x = (u16)((u32)raw_touch_x * 320 / MAX_12BIT);
			screen_touch_y = (u16)((u32)raw_touch_y * 240 / MAX_12BIT);
		}
	} else {
		touch_hid->pendown = false;
	}

	if(sync)
		input_sync(input);
}

static int touch_hid_probe(struct platform_device *pdev)
{
	int err;
	int i, j;
	struct device *dev;
	struct input_dev *input;
	struct regmap *map;
	struct touch_hid *touch_hid;

	dev = &pdev->dev;

	map = dev_get_regmap(dev->parent, NULL);
	if (!map)
		return -ENODEV;

	touch_hid = devm_kzalloc(dev, sizeof(*touch_hid), GFP_KERNEL);
	if (!touch_hid) {
		pr_err("failed to allocate memory for driver");
		return -ENOMEM;
	}

	input = devm_input_allocate_device(dev);
	if (!input){
		pr_err("failed to allocate input device");
		return -ENOMEM;
	}

	input_set_drvdata(input, touch_hid);
	input->name = "Nintendo 3DS touch HID";
	input->phys = DRIVER_NAME "/input0";

	input->dev.parent = dev;
	input->id.bustype = BUS_HOST;

	/* circle pad/mouse stuff */
	set_bit(EV_REL, input->evbit);
	set_bit(REL_X, input->relbit);
	set_bit(REL_Y, input->relbit);
	set_bit(REL_WHEEL, input->relbit);

	/* Enable VKB keys */
	set_bit(EV_KEY, input->evbit);
	input_set_capability(input, EV_MSC, MSC_SCAN);

	for (i = 0; i < VKB_ROWS; i++) {
		for (j = 0; j < VKB_COLS; j++) {
			if (vkb_map_keys[i][j])
				set_bit(vkb_map_keys[i][j], input->keybit);
		}
	}

	touch_hid->map = map;
	touch_hid->input_dev = input;
	platform_set_drvdata(pdev, touch_hid);

	err = touch_initialize(touch_hid->map);
	if (!err)
		err = touch_enable(touch_hid->map);

	if (err) {
		pr_err("failed to initialize hardware (%d)\n", err);
		return err;
	}

	err = input_setup_polling(input, touch_input_poll);
	if (err) {
		pr_err("failed to setup polling (%d)\n", err);
		return err;
	}

	input_set_poll_interval(input, POLL_INTERVAL_DEFAULT);

	err = input_register_device(input);
	if (err) {
		pr_err("failed to register input device (%d)\n", err);
		return err;
	}

	return 0;
}

static const struct of_device_id touch_hid_dt_ids[] = {
	{ .compatible = "nintendo," DRIVER_NAME, },
	{ }
};
MODULE_DEVICE_TABLE(of, touch_hid_dt_ids);

static struct platform_driver touch_hid_driver = {
	.probe = touch_hid_probe,

	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= of_match_ptr(touch_hid_dt_ids),
	},
};
module_platform_driver(touch_hid_driver);

MODULE_AUTHOR("Sergi Granell <xerpi.g.12@gmail.com>, Santiago Herrera");
MODULE_DESCRIPTION("Nintendo 3DS touchscreen/circlepad driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
