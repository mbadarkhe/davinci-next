/*
 * HID Sensor Time Driver
 * Copyright (c) 2012, Alexander Holler.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/hid-sensor-hub.h>
#include <linux/iio/iio.h>
#include <linux/rtc.h>

/* Format: HID-SENSOR-usage_id_in_hex */
/* Usage ID from spec for Time: 0x2000A0 */
#define DRIVER_NAME "HID-SENSOR-2000a0" /* must be lowercase */

static bool hid_time_hctosys_enabled = 1;
module_param_named(hctosys, hid_time_hctosys_enabled, bool, 0644);
MODULE_PARM_DESC(hctosys,
	"set the system time (once) if it is before 1970-01-02");
static bool hid_time_time_set_once;

enum hid_time_channel {
	CHANNEL_SCAN_INDEX_YEAR,
	CHANNEL_SCAN_INDEX_MONTH,
	CHANNEL_SCAN_INDEX_DAY,
	CHANNEL_SCAN_INDEX_HOUR,
	CHANNEL_SCAN_INDEX_MINUTE,
	CHANNEL_SCAN_INDEX_SECOND,
	CHANNEL_SCAN_INDEX_MILLISECOND, /* optional */
	TIME_RTC_CHANNEL_MAX,
};

struct hid_time_workts {
	struct work_struct work;
	struct hid_time_state *time_state;
};

struct hid_time_state {
	struct hid_sensor_hub_callbacks callbacks;
	struct hid_sensor_common common_attributes;
	struct hid_sensor_hub_attribute_info info[TIME_RTC_CHANNEL_MAX];
	struct rtc_time last_time;
	unsigned last_ms; /* == UINT_MAX to indicate ms aren't supported */
	spinlock_t lock_last_time;
	struct completion comp_last_time;
	struct rtc_time time_buf;
	unsigned ms_buf;
	struct rtc_device *rtc;
	struct hid_time_workts *workts;
};

static const u32 hid_time_addresses[TIME_RTC_CHANNEL_MAX] = {
	HID_USAGE_SENSOR_TIME_YEAR,
	HID_USAGE_SENSOR_TIME_MONTH,
	HID_USAGE_SENSOR_TIME_DAY,
	HID_USAGE_SENSOR_TIME_HOUR,
	HID_USAGE_SENSOR_TIME_MINUTE,
	HID_USAGE_SENSOR_TIME_SECOND,
	HID_USAGE_SENSOR_TIME_MILLISECOND, /* optional */
};

/* Channel names for verbose error messages */
static const char * const hid_time_channel_names[TIME_RTC_CHANNEL_MAX] = {
	"year", "month", "day", "hour", "minute", "second", "millisecond",
};

static void hid_time_hctosys(struct hid_time_state *time_state)
{
	struct timespec ts;
	char msbuf[5];
	int rc = rtc_valid_tm(&time_state->last_time);

	if (rc) {
		dev_err(time_state->rtc->dev.parent,
			"hctosys: invalid date/time\n");
		return;
	}

	getnstimeofday(&ts);
	if (ts.tv_sec >= 86400) /* 1970-01-02 00:00:00 UTC */
		/*
		 * Security: don't let a hot-pluggable device change
		 * a valid system time.
		 * In absence of a kernel-wide flag like 'systime_was_set',
		 * we check if the system time is earlier than 1970-01-02.
		 * Just using a flag inside the RTC subsystem (e.g. from
		 * hctosys) doesn't work, because the time could be set
		 * by userspace (NTP, date, user, ...) or something else
		 * in the kernel too.
		 * Using a whole day to decide if something else has set the
		 * time should be enough for even the longest boot to complete
		 * (including possible forced fscks) and load this module.
		 */
		return;

	if (time_state->last_ms != UINT_MAX) {
		ts.tv_nsec = time_state->last_ms * NSEC_PER_MSEC;
		snprintf(msbuf, sizeof(msbuf), ":%03d", time_state->last_ms);
	} else {
		ts.tv_nsec = NSEC_PER_SEC >> 1;
		*msbuf = 0;
	}
	rtc_tm_to_time(&time_state->last_time, &ts.tv_sec);
	rc = do_settimeofday(&ts);
	dev_info(time_state->rtc->dev.parent,
		"hctosys: setting system clock to "
		"%d-%02d-%02d %02d:%02d:%02d%s UTC (%u)\n",
		time_state->last_time.tm_year + 1900,
		time_state->last_time.tm_mon + 1,
		time_state->last_time.tm_mday,
		time_state->last_time.tm_hour,
		time_state->last_time.tm_min,
		time_state->last_time.tm_sec,
		msbuf,
		(unsigned int) ts.tv_sec);
}

/* Callback handler to send event after all samples are received and captured */
static int hid_time_proc_event(struct hid_sensor_hub_device *hsdev,
				unsigned usage_id, void *priv)
{
	unsigned long flags;
	struct hid_time_state *time_state = platform_get_drvdata(priv);

	spin_lock_irqsave(&time_state->lock_last_time, flags);
	time_state->last_time = time_state->time_buf;
	if (time_state->last_ms != UINT_MAX)
		time_state->last_ms = time_state->ms_buf;
	spin_unlock_irqrestore(&time_state->lock_last_time, flags);
	complete(&time_state->comp_last_time);
	if (unlikely(!hid_time_time_set_once && hid_time_hctosys_enabled)) {
		hid_time_time_set_once = 1;
		hid_time_hctosys(time_state);
	}
	return 0;
}

static u32 hid_time_value(size_t raw_len, char *raw_data)
{
	switch (raw_len) {
	case 1:
		return *(u8 *)raw_data;
	case 2:
		return *(u16 *)raw_data;
	case 4:
		return *(u32 *)raw_data;
	default:
		return (u32)(~0U); /* 0xff... or -1 to denote an error */
	}
}

static int hid_time_capture_sample(struct hid_sensor_hub_device *hsdev,
				unsigned usage_id, size_t raw_len,
				char *raw_data, void *priv)
{
	struct hid_time_state *time_state = platform_get_drvdata(priv);
	struct rtc_time *time_buf = &time_state->time_buf;

	switch (usage_id) {
	case HID_USAGE_SENSOR_TIME_YEAR:
		/*
		 * The draft for HID-sensors (HUTRR39) currently doesn't define
		 * the range for the year attribute. Therefor we support
		 * 8 bit (0-99) and 16 or 32 bits (full) as size for the year.
		 */
		if (raw_len == 1) {
			time_buf->tm_year = *(u8 *)raw_data;
			if (time_buf->tm_year < 70)
				/* assume we are in 1970...2069 */
				time_buf->tm_year += 100;
		} else
			time_buf->tm_year =
				(int)hid_time_value(raw_len, raw_data)-1900;
		break;
	case HID_USAGE_SENSOR_TIME_MONTH:
		/* sensors are sending the month as 1-12, we need 0-11 */
		time_buf->tm_mon = (int)hid_time_value(raw_len, raw_data)-1;
		break;
	case HID_USAGE_SENSOR_TIME_DAY:
		time_buf->tm_mday = (int)hid_time_value(raw_len, raw_data);
		break;
	case HID_USAGE_SENSOR_TIME_HOUR:
		time_buf->tm_hour = (int)hid_time_value(raw_len, raw_data);
		break;
	case HID_USAGE_SENSOR_TIME_MINUTE:
		time_buf->tm_min = (int)hid_time_value(raw_len, raw_data);
		break;
	case HID_USAGE_SENSOR_TIME_SECOND:
		time_buf->tm_sec = (int)hid_time_value(raw_len, raw_data);
		break;
	case HID_USAGE_SENSOR_TIME_MILLISECOND:
		time_state->ms_buf = hid_time_value(raw_len, raw_data);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* small helper, haven't found any other way */
static const char *hid_time_attrib_name(u32 attrib_id)
{
	static const char unknown[] = "unknown";
	unsigned i;

	for (i = 0; i < TIME_RTC_CHANNEL_MAX; ++i) {
		if (hid_time_addresses[i] == attrib_id)
			return hid_time_channel_names[i];
	}
	return unknown; /* should never happen */
}

static int hid_time_parse_report(struct platform_device *pdev,
				struct hid_sensor_hub_device *hsdev,
				unsigned usage_id,
				struct hid_time_state *time_state)
{
	int report_id, i;

	/* Check if all required attributes are available */
	for (i = 0; i < CHANNEL_SCAN_INDEX_MILLISECOND; ++i)
		if (sensor_hub_input_get_attribute_info(hsdev,
				HID_INPUT_REPORT, usage_id,
				hid_time_addresses[i],
				&time_state->info[i]) < 0)
			return -EINVAL;
	if (!sensor_hub_input_get_attribute_info(hsdev, HID_INPUT_REPORT,
		usage_id, hid_time_addresses[i], &time_state->info[i])) {
		dev_info(&pdev->dev, "milliseconds supported\n");
		time_state->last_ms = 0;
	}
	/* Check the (needed) attributes for sanity */
	report_id = time_state->info[0].report_id;
	if (report_id < 0) {
		dev_err(&pdev->dev, "bad report ID!\n");
		return -EINVAL;
	}
	for (i = 0; i < TIME_RTC_CHANNEL_MAX; ++i) {
		if (time_state->info[i].attrib_id ==
				HID_USAGE_SENSOR_TIME_MILLISECOND &&
				time_state->last_ms == UINT_MAX)
			continue;
		if (time_state->info[i].report_id != report_id) {
			dev_err(&pdev->dev,
				"not all needed attributes inside the same report!\n");
			return -EINVAL;
		}
		if (time_state->info[i].size == 3 ||
				time_state->info[i].size > 4) {
			dev_err(&pdev->dev,
				"attribute '%s' not 8, 16 or 32 bits wide!\n",
				hid_time_attrib_name(
					time_state->info[i].attrib_id));
			return -EINVAL;
		}
		if (time_state->info[i].units !=
				HID_USAGE_SENSOR_UNITS_NOT_SPECIFIED &&
				/* allow attribute seconds with unit seconds */
				!(time_state->info[i].attrib_id ==
				HID_USAGE_SENSOR_TIME_SECOND &&
				time_state->info[i].units ==
				HID_USAGE_SENSOR_UNITS_SECOND)) {
			dev_err(&pdev->dev,
				"attribute '%s' hasn't a unit of type 'none'!\n",
				hid_time_attrib_name(
					time_state->info[i].attrib_id));
			return -EINVAL;
		}
		if (time_state->info[i].unit_expo) {
			dev_err(&pdev->dev,
				"attribute '%s' hasn't a unit exponent of 1!\n",
				hid_time_attrib_name(
					time_state->info[i].attrib_id));
			return -EINVAL;
		}
	}

	return 0;
}

static int hid_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	unsigned long flags;
	struct hid_time_state *time_state =
		platform_get_drvdata(to_platform_device(dev));
	int ret;

	INIT_COMPLETION(time_state->comp_last_time);
	/* get a report with all values through requesting one value */
	sensor_hub_input_attr_get_raw_value(time_state->common_attributes.hsdev,
			HID_USAGE_SENSOR_TIME, hid_time_addresses[0],
			time_state->info[0].report_id);
	/* wait for all values (event) */
	ret = wait_for_completion_killable_timeout(
			&time_state->comp_last_time, HZ*6);
	if (ret > 0) {
		/* no error */
		spin_lock_irqsave(&time_state->lock_last_time, flags);
		*tm = time_state->last_time;
		spin_unlock_irqrestore(&time_state->lock_last_time, flags);
		return 0;
	}
	if (!ret)
		return -EIO; /* timeouted */
	return ret; /* killed (-ERESTARTSYS) */
}

static const struct rtc_class_ops hid_time_rtc_ops = {
	.read_time = hid_rtc_read_time,
};

static void hid_time_request_report_work(struct work_struct *work)
{
	struct hid_time_state *time_state =
		container_of(work, struct hid_time_workts, work)
			->time_state;
	/* get a report with all values through requesting one value */
	sensor_hub_input_attr_get_raw_value(
		time_state->common_attributes.hsdev, HID_USAGE_SENSOR_TIME,
		hid_time_addresses[0], time_state->info[0].report_id);
	time_state->workts = NULL;
	kfree(work);
}

static int hid_time_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct hid_sensor_hub_device *hsdev = pdev->dev.platform_data;
	struct hid_time_state *time_state = devm_kzalloc(&pdev->dev,
		sizeof(struct hid_time_state), GFP_KERNEL);

	if (time_state == NULL)
		return -ENOMEM;

	time_state->last_ms = UINT_MAX;

	platform_set_drvdata(pdev, time_state);

	spin_lock_init(&time_state->lock_last_time);
	init_completion(&time_state->comp_last_time);
	time_state->common_attributes.hsdev = hsdev;
	time_state->common_attributes.pdev = pdev;

	ret = hid_sensor_parse_common_attributes(hsdev,
				HID_USAGE_SENSOR_TIME,
				&time_state->common_attributes);
	if (ret) {
		dev_err(&pdev->dev, "failed to setup common attributes!\n");
		return ret;
	}

	ret = hid_time_parse_report(pdev, hsdev, HID_USAGE_SENSOR_TIME,
					time_state);
	if (ret) {
		dev_err(&pdev->dev, "failed to setup attributes!\n");
		return ret;
	}

	time_state->callbacks.send_event = hid_time_proc_event;
	time_state->callbacks.capture_sample = hid_time_capture_sample;
	time_state->callbacks.pdev = pdev;
	ret = sensor_hub_register_callback(hsdev, HID_USAGE_SENSOR_TIME,
					&time_state->callbacks);
	if (ret < 0) {
		dev_err(&pdev->dev, "register callback failed!\n");
		return ret;
	}

	time_state->rtc = devm_rtc_device_register(&pdev->dev,
					"hid-sensor-time", &hid_time_rtc_ops,
					THIS_MODULE);

	if (IS_ERR(time_state->rtc)) {
		dev_err(&pdev->dev, "rtc device register failed!\n");
		return PTR_ERR(time_state->rtc);
	}

	if (!hid_time_time_set_once && hid_time_hctosys_enabled) {
		/*
		 * Request a HID report to set the time.
		 * Calling sensor_hub_..._get_raw_value() here directly
		 * doesn't work, therefor we have to use a work.
		 */
		time_state->workts = kmalloc(sizeof(struct hid_time_workts),
			GFP_KERNEL);
		if (time_state->workts == NULL)
			return -ENOMEM;
		time_state->workts->time_state = time_state;
		INIT_WORK(&time_state->workts->work,
			hid_time_request_report_work);
		schedule_work(&time_state->workts->work);
	}

	return ret;
}

static int hid_time_remove(struct platform_device *pdev)
{
	struct hid_sensor_hub_device *hsdev = pdev->dev.platform_data;
	struct hid_time_state *time_state = platform_get_drvdata(pdev);

	if (time_state->workts) {
		cancel_work_sync(&time_state->workts->work);
		kfree(time_state->workts);
		time_state->workts = NULL;
	}
	sensor_hub_remove_callback(hsdev, HID_USAGE_SENSOR_TIME);

	return 0;
}

static struct platform_driver hid_time_platform_driver = {
	.driver = {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
	.probe		= hid_time_probe,
	.remove		= hid_time_remove,
};
module_platform_driver(hid_time_platform_driver);

MODULE_DESCRIPTION("HID Sensor Time");
MODULE_AUTHOR("Alexander Holler <holler@ahsoftware.de>");
MODULE_LICENSE("GPL");
