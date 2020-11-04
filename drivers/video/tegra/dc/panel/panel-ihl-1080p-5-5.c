/*
 * panel-ihl-1080p-5_5.c: Panel driver for Visionox 5.5" AMOLED panel.
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>

#include "../dc.h"
#include "../dc_priv.h"

#include "board.h"
#include "board-panel.h"

#define DSI_PANEL_RESET		1

static bool reg_requested;
static struct regulator *avdd_lcd_3v3;
static struct device *dc_dev;
static u16 en_panel_rst_n;

static int dsi_ihl_1080p_5_5_regulator_get(struct device *dev)
{
	int err = 0;

	if (reg_requested)
		return 0;
	avdd_lcd_3v3 = regulator_get(dev, "avdd_lcd_3v3");
	if (IS_ERR_OR_NULL(avdd_lcd_3v3)) {
		pr_err("avdd_lcd_3v3 regulator get failed\n");
		err = PTR_ERR(avdd_lcd_3v3);
		avdd_lcd_3v3 = NULL;
		goto fail;
	}

	reg_requested = true;
	return 0;
fail:
	return err;
}

static int dsi_ihl_1080p_5_5_enable(struct device *dev)
{
	int err = 0;

	err = tegra_panel_check_regulator_dt_support("ihl,1080p-5-5",
		&panel_of);
	if (err < 0) {
		pr_err("display regulator dt check failed\n");
		goto fail;
	} 

	err = dsi_ihl_1080p_5_5_regulator_get(dev);
	if (err < 0) {
		pr_err("dsi regulator get failed\n");
		goto fail;
	}

	err = tegra_panel_gpio_get_dt("ihl,1080p-5-5", &panel_of);
	if (err < 0) {
		pr_err("display gpio get failed\n");
		goto fail;
	}

	if (gpio_is_valid(panel_of.panel_gpio[TEGRA_GPIO_RESET]))
		en_panel_rst_n = panel_of.panel_gpio[TEGRA_GPIO_RESET];
	else {
		pr_err("display reset gpio invalid\n");
		goto fail;
	}

	if (avdd_lcd_3v3) {
                err = regulator_enable(avdd_lcd_3v3);
	        if (err < 0) {
			pr_err("avdd_lcd_3v3 regulator enable failed\n");
			goto fail;
		}
	}

	usleep_range(10000, 11000);


	err = gpio_direction_output(en_panel_rst_n, 1);
	if (err < 0) {
		pr_err("setting display reset gpio value failed\n");
		goto fail;
	}

	usleep_range(15000, 15500);

	dc_dev = dev;
	return 0;
fail:
	return err;
}

static int dsi_ihl_1080p_5_5_disable(struct device *dev)
{
	if (gpio_is_valid(en_panel_rst_n)) {
		/* Wait for 50ms before triggering panel reset */
		msleep(50);
		gpio_set_value(en_panel_rst_n, 0);
		usleep_range(500, 1000);
	} else
		pr_err("ERROR! display reset gpio invalid\n");

	usleep_range(2000, 2500);

	if (avdd_lcd_3v3)
		regulator_disable(avdd_lcd_3v3);
	/* Min delay of 140ms required to avoid turning
	 * the panel on too soon after power off */
	msleep(140);

	dc_dev = NULL;

	return 0;
}

static int dsi_ihl_1080p_5_5_postsuspend(void)
{
	return 0;
}


struct tegra_panel_ops dsi_ihl_1080p_5_5_ops = {
	.enable = dsi_ihl_1080p_5_5_enable,
	.disable = dsi_ihl_1080p_5_5_disable,
	.postsuspend = dsi_ihl_1080p_5_5_postsuspend,
};
