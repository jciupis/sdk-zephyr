/* ST Microelectronics IIS2MDC 3-axis magnetometer sensor
 *
 * Copyright (c) 2020 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Datasheet:
 * https://www.st.com/resource/en/datasheet/iis2mdc.pdf
 */

#define DT_DRV_COMPAT st_iis2mdc

#include <kernel.h>
#include <drivers/sensor.h>
#include <drivers/gpio.h>
#include <logging/log.h>
#include "iis2mdc.h"

LOG_MODULE_DECLARE(IIS2MDC, CONFIG_SENSOR_LOG_LEVEL);

static int iis2mdc_enable_int(struct device *dev, int enable)
{
	struct iis2mdc_data *iis2mdc = dev->driver_data;

	/* set interrupt on mag */
	return iis2mdc_drdy_on_pin_set(iis2mdc->ctx, enable);
}

/* link external trigger to event data ready */
int iis2mdc_trigger_set(struct device *dev,
			  const struct sensor_trigger *trig,
			  sensor_trigger_handler_t handler)
{
	struct iis2mdc_data *iis2mdc = dev->driver_data;
	union axis3bit16_t raw;

	if (trig->chan == SENSOR_CHAN_MAGN_XYZ) {
		iis2mdc->handler_drdy = handler;
		if (handler) {
			/* fetch raw data sample: re-trigger lost interrupt */
			iis2mdc_magnetic_raw_get(iis2mdc->ctx, raw.u8bit);

			return iis2mdc_enable_int(dev, 1);
		} else {
			return iis2mdc_enable_int(dev, 0);
		}
	}

	return -ENOTSUP;
}

/* handle the drdy event: read data and call handler if registered any */
static void iis2mdc_handle_interrupt(void *arg)
{
	struct device *dev = arg;
	struct iis2mdc_data *iis2mdc = dev->driver_data;
	const struct iis2mdc_config *const config =
						dev->config_info;
	struct sensor_trigger drdy_trigger = {
		.type = SENSOR_TRIG_DATA_READY,
	};

	if (iis2mdc->handler_drdy != NULL) {
		iis2mdc->handler_drdy(dev, &drdy_trigger);
	}

	gpio_pin_interrupt_configure(iis2mdc->gpio, config->drdy_pin,
				     GPIO_INT_EDGE_TO_ACTIVE);
}

static void iis2mdc_gpio_callback(struct device *dev,
				    struct gpio_callback *cb, u32_t pins)
{
	struct iis2mdc_data *iis2mdc =
		CONTAINER_OF(cb, struct iis2mdc_data, gpio_cb);
	const struct iis2mdc_config *const config = dev->config_info;

	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure(dev, config->drdy_pin, GPIO_INT_DISABLE);

#if defined(CONFIG_IIS2MDC_TRIGGER_OWN_THREAD)
	k_sem_give(&iis2mdc->gpio_sem);
#elif defined(CONFIG_IIS2MDC_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&iis2mdc->work);
#endif
}

#ifdef CONFIG_IIS2MDC_TRIGGER_OWN_THREAD
static void iis2mdc_thread(int dev_ptr, int unused)
{
	struct device *dev = INT_TO_POINTER(dev_ptr);
	struct iis2mdc_data *iis2mdc = dev->driver_data;

	ARG_UNUSED(unused);

	while (1) {
		k_sem_take(&iis2mdc->gpio_sem, K_FOREVER);
		iis2mdc_handle_interrupt(dev);
	}
}
#endif

#ifdef CONFIG_IIS2MDC_TRIGGER_GLOBAL_THREAD
static void iis2mdc_work_cb(struct k_work *work)
{
	struct iis2mdc_data *iis2mdc =
		CONTAINER_OF(work, struct iis2mdc_data, work);

	iis2mdc_handle_interrupt(iis2mdc->dev);
}
#endif

int iis2mdc_init_interrupt(struct device *dev)
{
	struct iis2mdc_data *iis2mdc = dev->driver_data;
	const struct iis2mdc_config *const config = dev->config_info;

	/* setup data ready gpio interrupt */
	iis2mdc->gpio = device_get_binding(config->drdy_port);
	if (iis2mdc->gpio == NULL) {
		LOG_DBG("Cannot get pointer to %s device",
			    config->drdy_port);
		return -EINVAL;
	}

#if defined(CONFIG_IIS2MDC_TRIGGER_OWN_THREAD)
	k_sem_init(&iis2mdc->gpio_sem, 0, UINT_MAX);
	k_thread_create(&iis2mdc->thread, iis2mdc->thread_stack,
			CONFIG_IIS2MDC_THREAD_STACK_SIZE,
			(k_thread_entry_t)iis2mdc_thread, dev,
			0, NULL, K_PRIO_COOP(CONFIG_IIS2MDC_THREAD_PRIORITY),
			0, K_NO_WAIT);
#elif defined(CONFIG_IIS2MDC_TRIGGER_GLOBAL_THREAD)
	iis2mdc->work.handler = iis2mdc_work_cb;
	iis2mdc->dev = dev;
#endif

	gpio_pin_configure(iis2mdc->gpio, config->drdy_pin,
			   GPIO_INPUT | config->drdy_flags);

	gpio_init_callback(&iis2mdc->gpio_cb,
			   iis2mdc_gpio_callback,
			   BIT(config->drdy_pin));

	if (gpio_add_callback(iis2mdc->gpio, &iis2mdc->gpio_cb) < 0) {
		LOG_DBG("Could not set gpio callback");
		return -EIO;
	}

	return gpio_pin_interrupt_configure(iis2mdc->gpio, config->drdy_pin,
					    GPIO_INT_EDGE_TO_ACTIVE);
}
