/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include "mtk_musb.h"
#include "musb_core.h"
#include "usb20.h"

/*#include <mach/mt_gpio.h>*/

#define FRA (48)
#define PARA (28)

#ifdef FPGA_PLATFORM
bool usb_enable_clock(bool enable)
{
	return true;
}

void usb_phy_poweron(void)
{
}

void usb_phy_savecurrent(void)
{
}

void usb_phy_recover(void)
{
}

/* BC1.2*/
void Charger_Detect_Init(void)
{
}

void Charger_Detect_Release(void)
{
}

void usb_phy_context_save(void)
{
}

void usb_phy_context_restore(void)
{
}

#ifdef CONFIG_MTK_UART_USB_SWITCH
bool usb_phy_check_in_uart_mode(void)
{
	UINT8 usb_port_mode;

	usb_enable_clock(true);
	udelay(50);

	usb_port_mode = USB_PHY_Read_Register8(0x6B);
	usb_enable_clock(false);

	if ((usb_port_mode == 0x5C) || (usb_port_mode == 0x5E))
		return true;
	else
		return false;
}
void usb_phy_switch_to_usb(void)
{
	usb_enable_clock(true);
	udelay(50);
	/* clear force_uart_en */
	USBPHY_WRITE8(0x6B, 0x00);
	usb_enable_clock(false);
	usb_phy_poweron();
	/* disable the USB clock turned on in usb_phy_poweron() */
	usb_enable_clock(false);
}

void usb_phy_switch_to_uart(void)
{

	if (usb_phy_check_in_uart_mode())
		return;
	DBG(0, "Mask PMIC charger detection in UART mode.\n");
	/*ALPS00775710 */
	/*ALPS00775710*/

	usb_enable_clock(true);
	udelay(50);

	/* RG_USB20_BC11_SW_EN = 1'b0 */
	USBPHY_CLR8(0x1a, 0x80);

	/* Set RG_SUSPENDM to 1 */
	USBPHY_SET8(0x68, 0x08);

	/* force suspendm = 1 */
	USBPHY_SET8(0x6a, 0x04);

	/* Set ru_uart_mode to 2'b01 */
	USBPHY_SET8(0x6B, 0x5C);

	/* Set RG_UART_EN to 1 */
	USBPHY_SET8(0x6E, 0x07);

	/* Set RG_USB20_DM_100K_EN to 1 */
	USBPHY_SET8(0x22, 0x02);
	usb_enable_clock(false);
}

#endif

#else

#ifdef CONFIG_MTK_UART_USB_SWITCH
bool in_uart_mode;
#endif

static DEFINE_SPINLOCK(musb_reg_clock_lock);
static DEFINE_SPINLOCK(musb_phy_clock_lock);
static DEFINE_SPINLOCK(musb_mcu_clock_lock);

void enable_phy_clock(bool enable)
{
	static int count;
	unsigned long flags;

	spin_lock_irqsave(&musb_phy_clock_lock, flags);
	/* USB phy 48M clock , UNIVPLL_CON0[26] */
	if (enable) {
		if (count == 0) {
			/*DBG(0, "enable phy clock\n");*/
			clk_enable(usbpll_clk);
		}
		count++;
	} else {
		if (count == 1) {
			/*DBG(0, "disable phy clock\n");*/
			clk_disable(usbpll_clk);
		}
		count = (count == 0) ? 0 : (count - 1);
	}
	spin_unlock_irqrestore(&musb_phy_clock_lock, flags);
}

void enable_mcu_clock(bool enable)
{
	static int count;
	unsigned long flags;

	spin_lock_irqsave(&musb_mcu_clock_lock, flags);
	/* USB phy 48M clock , UNIVPLL_CON0[26] */
	if (enable) {
		if (count == 0) {
			/*DBG(0, "enable mcu clock\n");*/
			clk_enable(usbmcu_clk);
		}
		count++;
	} else {
		if (count == 1) {
			/*DBG(0, "disable mcu clock\n");*/
			clk_disable(usbmcu_clk);
		}
		count = (count == 0) ? 0 : (count - 1);
	}
	spin_unlock_irqrestore(&musb_mcu_clock_lock, flags);
}

bool usb_enable_clock(bool enable)
{
	static int count;
	unsigned long flags;

	spin_lock_irqsave(&musb_reg_clock_lock, flags);

	if (enable && count == 0) {
		enable_phy_clock(true);
		/*DBG(0, "enable usb0 clock\n");*/
		clk_enable(usb_clk);
		enable_mcu_clock(true);
	} else if (!enable && count == 1) {
		/*DBG(0, "disable usb0 clock\n");*/
		clk_disable(usb_clk);
		enable_mcu_clock(false);
		enable_phy_clock(false);
	}

	if (enable)
		count++;
	else
		count = (count == 0) ? 0 : (count-1);

	spin_unlock_irqrestore(&musb_reg_clock_lock, flags);

	return 1;
}

static void hs_slew_rate_cal(void)
{
	unsigned long data;
	unsigned long x;
	unsigned char value;
	unsigned long start_time, timeout;
	unsigned int timeout_flag = 0;
	/*4 s1:enable usb ring oscillator.*/
	USBPHY_WRITE8(0x15, 0x80);

	/*4 s2:wait 1us.*/
	udelay(1);

	/*4 s3:enable free run clock*/
	USBPHY_WRITE8(0xf00-0x800+0x11, 0x01);
	/*4 s4:setting cyclecnt.*/
	USBPHY_WRITE8(0xf00-0x800+0x01, 0x04);
	/*4 s5:enable frequency meter*/
	USBPHY_SET8(0xf00-0x800+0x03, 0x01);

	/*4 s6:wait for frequency valid.*/
	start_time = jiffies;
	timeout = jiffies + 3 * HZ;

	while (!(USBPHY_READ8(0xf00-0x800+0x10)&0x1)) {
		if (time_after(jiffies, timeout)) {
			timeout_flag = 1;
			break;
		}
	}

	/*4 s7: read result.*/
	if (timeout_flag) {
		DBG(0, "[USBPHY] Slew Rate Calibration: Timeout\n");
		value = 0x4;
	}	else{
		data = USBPHY_READ32(0xf00-0x800+0x0c);
		x = ((1024*FRA*PARA)/data);
		value = (unsigned char)(x/1000);
		if ((x-value*1000)/100 >= 5)
			value += 1;
		DBG(0, "[USBPHY]slew calibration: FM_OUT = %lu, x= %lu, value= %d\n", data, x, value);
	}

	/*4 s8: disable Frequency and run clock.*/
	USBPHY_CLR8(0xf00-0x800+0x03, 0x01);/*disable frequency meter*/
	USBPHY_CLR8(0xf00-0x800+0x11, 0x01);/*disable free run clock*/

	/*4 s9:*/
	USBPHY_WRITE8(0x15, value<<4);

	/*4 s10:disable usb ring oscillator.*/
	USBPHY_CLR8(0x15, 0x80);
}

#ifdef CONFIG_MTK_UART_USB_SWITCH
u32 usb_port_mode_temp;
bool usb_phy_check_in_uart_mode(void)
{
	UINT8 usb_port_mode;

	usb_enable_clock(true);
	udelay(50);
	usb_port_mode = USBPHY_READ8(0x6B);
	/*usb_port_mode = 1;*/
	usb_enable_clock(false);

	if ((usb_port_mode == 0x5C) ||
		(usb_port_mode == 0x5E) || (usb_port_mode_temp == 1)) {
		usb_port_mode_temp = 1;
		return true;
	} else
		return false;
}

void usb_phy_switch_to_uart(void)
{
	/*if (usb_phy_check_in_uart_mode()) */
		/*return;*/
	DBG(0, "USB Port -> UART\n");
	#ifdef CONFIG_MTK_MUSB_PORT0_LOWPOWER_MODE
	mt_usb_clock_prepare();
	#endif
	usb_enable_clock(true);
	udelay(50);

	USBPHY_SET8(0x00, 0x01);
	USBPHY_CLR8(0x1a, 0x80);/*818 bit23*/
	USBPHY_SET8(0x1d, 0x40);/*81C bit14*/
	USBPHY_CLR8(0x1d, 0x20);/*81C bit13*/
	USBPHY_SET8(0x1d, 0x10);/*81C bit12*/
	USBPHY_SET8(0x1d, 0x08);/*81C bit11*/
	USBPHY_CLR8(0x1d, 0x04);/*81C bit10*/
	USBPHY_SET8(0x6e, 0x01);
	USBPHY_SET8(0x6b, 0x40);
	USBPHY_CLR8(0x6b, 0x80);
	USBPHY_SET8(0x22, 0x02);/*820 bit17*/
	USBPHY_SET8(0x6b, 0x04);
	USBPHY_SET8(0x6e, 0x04);
	USBPHY_SET8(0x6b, 0x10);

	regmap_update_bits(mt_regmap, 0x980, 1<<7, 1<<7);
#if 0
	/* RG_USB20_BC11_SW_EN = 1'b0 */
	USBPHY_CLR8(0x1a, 0x80);

	/* Set RG_SUSPENDM to 1 */
	USBPHY_SET8(0x68, 0x08);

	/* force suspendm = 1 */
	USBPHY_SET8(0x6a, 0x04);

	/* Set ru_uart_mode to 2'b01 */
	USBPHY_SET8(0x6B, 0x5C);

	/* Set RG_UART_EN to 1 */
	USBPHY_SET8(0x6E, 0x07);

	/* Set RG_USB20_DM_100K_EN to 1 */
	USBPHY_SET8(0x22, 0x02);
#endif
	usb_enable_clock(false);

	#ifdef CONFIG_MTK_MUSB_PORT0_LOWPOWER_MODE
	mt_usb_clock_unprepare();
	#endif
	/*set uart rx path*/
	/*mtk_uart_usb_rx_sel(1, 1);*/
	usb_port_mode_temp = 1;
	DBG(0, "usb port value in uart function:%d\n", usb_port_mode_temp);
	/* GPIO Selection */
	/* DRV_WriteReg32(GPIO_BASE + 0x504, 0x10);	//set */
}


void usb_phy_switch_to_usb(void)
{
#ifdef CONFIG_MTK_MUSB_PORT0_LOWPOWER_MODE
	mt_usb_clock_prepare();
#endif
	usb_enable_clock(true);
	udelay(50);

	regmap_update_bits(mt_regmap, 0x980, 1<<7, 0<<7);
	USBPHY_CLR8(0x00, 0x01);
	USBPHY_SET8(0x1a, 0x80);/*818 bit23*/
	USBPHY_CLR8(0x1d, 0x40);/*81C bit14*/
	USBPHY_SET8(0x1d, 0x20);/*81C bit13*/
	USBPHY_CLR8(0x1d, 0x10);/*81C bit12*/
	USBPHY_CLR8(0x1d, 0x08);/*81C bit11*/
	USBPHY_SET8(0x1d, 0x04);/*81C bit10*/
	USBPHY_CLR8(0x6e, 0x01);
	USBPHY_CLR8(0x6b, 0x40);
	USBPHY_SET8(0x6b, 0x80);
	USBPHY_CLR8(0x22, 0x02);/*820 bit17*/
	USBPHY_CLR8(0x6b, 0x04);
	USBPHY_CLR8(0x6e, 0x04);
	USBPHY_CLR8(0x6b, 0x10);
#if 0
	/* GPIO Selection */
	/*DRV_WriteReg32(GPIO_BASE + 0x508, 0x10);		//set */
	mtk_uart_usb_rx_sel(1, 0);
	usb_enable_clock(true);
	udelay(50);
	/* clear force_uart_en */
	USBPHY_WRITE8(0x6B, 0x00);
#endif
	/*usb_enable_clock(false);*/
	/*usb_phy_poweron();*/
	/* disable the USB clock turned on in usb_phy_poweron() */
	/*usb_enable_clock(false);*/

	usb_port_mode_temp = 0;
	DBG(0, "usb port value in usb function:%d\n", usb_port_mode_temp);
}
#endif

void usb_phy_poweron(void)
{
	#ifdef CONFIG_MTK_UART_USB_SWITCH
	if (usb_phy_check_in_uart_mode())
		return;
	#endif

	/* enable USB MAC clock. */
	usb_enable_clock(true);

	/* wait 50 usec for PHY3.3v/1.8v stable. */
	udelay(50);

	/* force_uart_en, 1'b0 */
	USBPHY_CLR8(0x6b, 0x04);
	/* RG_UART_EN, 1'b0 */
	USBPHY_CLR8(0x6e, 0x01);
	/* rg_usb20_gpio_ctl, 1'b0, usb20_gpio_mode, 1'b0 */
	USBPHY_CLR8(0x21, 0x03);
	/*USBPHY_CLR8(0x21, 0x01);*/

	/* RG_USB20_BC11_SW_EN, 1'b0 */
	USBPHY_CLR8(0x1a, 0x80);

	/* rg_usb20_dp_100k_mode, 1'b1 */
	USBPHY_SET8(0x22, 0x04);
	USBPHY_CLR8(0x22, 0x03);

	/*OTG enable*/
	USBPHY_SET8(0x1a, 0x10);
	/* 6 s6: release force suspendm. */
	USBPHY_CLR8(0x6a, 0x04);

	/*6-1. PASS RX sensitivity HQA requirement*/
	USBPHY_SET8(0x18, 0x06);

	/* 7 s7: wait for 800 usec. */
	udelay(800);

	/* force enter device mode, from K2, FIXME */
	USBPHY_CLR8(0x6c, 0x10);
	USBPHY_SET8(0x6c, 0x2E);
	USBPHY_SET8(0x6d, 0x3E);

	DBG(0, "usb power on success\n");
}

#ifdef CONFIG_MTK_UART_USB_SWITCH
static bool skipDisableUartMode;
#endif

static void usb_phy_savecurrent_internal(void)
{

	/* 4 1. swtich to USB function. (system register, force ip into usb mode. */

	#ifdef CONFIG_MTK_UART_USB_SWITCH
	if (!usb_phy_check_in_uart_mode()) {
		/* enable USB MAC clock. */
		usb_enable_clock(true);

		/* wait 50 usec for PHY3.3v/1.8v stable. */
		udelay(50);

		/* force_uart_en, 1'b0 */
		USBPHY_CLR8(0x6b, 0x04);
		/* RG_UART_EN, 1'b0 */
		USBPHY_CLR8(0x6e, 0x01);
		/* rg_usb20_gpio_ctl, 1'b0, usb20_gpio_mode, 1'b0 */
		USBPHY_CLR8(0x21, 0x03);

		/*4 2. release force suspendm.*/
		/*USBPHY_CLR8(0x6a, 0x04);*/
		USBPHY_SET8(0x6a, 0x04);
		/* RG_SUSPENDM, 1'b1 */
		USBPHY_SET8(0x68, 0x08);
		usb_enable_clock(false);
	} else {
		if (skipDisableUartMode)
			skipDisableUartMode = false;
		else
			return;
	}
	#else
	/* force_uart_en, 1'b0 */
	USBPHY_CLR8(0x6b, 0x04);
	/* RG_UART_EN, 1'b0 */
	USBPHY_CLR8(0x6e, 0x01);
	/* rg_usb20_gpio_ctl, 1'b0, usb20_gpio_mode, 1'b0 */
	USBPHY_CLR8(0x21, 0x03);

	/* RG_USB20_BC11_SW_EN, 1'b0 */
	/* USBPHY_CLR8(0x6a, 0x04);*/
	USBPHY_SET8(0x6a, 0x04);
	USBPHY_SET8(0x68, 0x08);
	#endif

	/* RG_DPPULLDOWN, 1'b1, RG_DMPULLDOWN, 1'b1 */
	USBPHY_SET8(0x68, 0xc0);
	/* RG_XCVRSEL[1:0], 2'b01. */
	USBPHY_CLR8(0x68, 0x30);
	USBPHY_SET8(0x68, 0x10);
	/* RG_TERMSEL, 1'b1 */
	USBPHY_SET8(0x68, 0x04);
	/* RG_DATAIN[3:0], 4'b0000 */
	USBPHY_CLR8(0x69, 0x3c);

	/* force_dp_pulldown, 1'b1, force_dm_pulldown, 1'b1,
	* force_xcversel, 1'b1, force_termsel, 1'b1, force_datain, 1'b1
	*/
	USBPHY_SET8(0x6a, 0xba);

	/*4 8.RG_USB20_BC11_SW_EN 1'b0*/
	USBPHY_CLR8(0x1a, 0x80);
	/*4 9.RG_USB20_OTG_VBUSSCMP_EN 1'b0*/
	USBPHY_CLR8(0x1a, 0x10);
	/*4 10. delay 800us.*/
	udelay(800);
	/*4 11. rg_usb20_pll_stable = 1*/
	USBPHY_CLR8(0x68, 0x08);
	/*
	*	USBPHY_SET8(0x63, 0x02);
	*
	*	ALPS00427972, implement the analog register formula
	*	ALPS00427972, implement the analog register formula
	*/

	udelay(1);
	/*4 12.  force suspendm = 1.*/
	/* USBPHY_SET8(0x6a, 0x04);*/
	/*4 13.  wait 1us*/
	udelay(1);

	/* force enter device mode, from K2, FIXME */
	/* force enter device mode */
	/*USBPHY_CLR8(0x6c, 0x10);*/
	/*USBPHY_SET8(0x6c, 0x2E);*/
	/*USBPHY_SET8(0x6d, 0x3E);*/
}

void usb_phy_savecurrent(void)
{
	usb_phy_savecurrent_internal();
	/* 4 14. turn off internal 48Mhz PLL. */
	usb_enable_clock(false);
	DBG(0, "usb save current success\n");
}

void usb_phy_recover(void)
{

	/* turn on USB reference clock. */
	usb_enable_clock(true);
	/* wait 50 usec. */
	udelay(50);

	#ifdef CONFIG_MTK_UART_USB_SWITCH
	if (!usb_phy_check_in_uart_mode()) {
		/* clean PUPD_BIST_EN */
		/* PUPD_BIST_EN = 1'b0 */
		/* PMIC will use it to detect charger type */
		USBPHY_CLR8(0x1d, 0x10);

		/* force_uart_en, 1'b0 */
		USBPHY_CLR8(0x6b, 0x04);
		/* RG_UART_EN, 1'b0 */
		USBPHY_CLR8(0x6e, 0x1);
		/* force_suspendm, 1'b0 */
		USBPHY_CLR8(0x6a, 0x04);
		USBPHY_CLR8(0x22, 0x02);

		skipDisableUartMode = false;
	} else {
		/*if (!skipDisableUartMode)*/
			return;
	}
	#else
	/* clean PUPD_BIST_EN */
	/* PUPD_BIST_EN = 1'b0 */
	/* PMIC will use it to detect charger type */
	USBPHY_CLR8(0x1d, 0x10);

	/* force_uart_en, 1'b0 */
	USBPHY_CLR8(0x6b, 0x04);
	/* RG_UART_EN, 1'b0 */
	USBPHY_CLR8(0x6e, 0x1);
	/* rg_usb20_gpio_ctl, 1'b0, usb20_gpio_mode, 1'b0 */
	/* force_suspendm, 1'b0 */
	USBPHY_CLR8(0x6a, 0x04);

	USBPHY_CLR8(0x21, 0x03);
	#endif

	/* RG_DPPULLDOWN, 1'b0, RG_DMPULLDOWN, 1'b0 */
	USBPHY_CLR8(0x68, 0x40);
	/* 4 7. RG_DMPULLDOWN = 1'b0 */
	USBPHY_CLR8(0x68, 0x80);
	/* RG_XCVRSEL[1:0], 2'b00. */
	USBPHY_CLR8(0x68, 0x30);
	/* RG_TERMSEL, 1'b0 */
	USBPHY_CLR8(0x68, 0x04);
	/* RG_DATAIN[3:0], 4'b0000 */
	USBPHY_CLR8(0x69, 0x3c);

	/* force_dp_pulldown, 1'b0, force_dm_pulldown, 1'b0*/
	USBPHY_CLR8(0x6a, 0x10);
	/* 4 12. force_dm_pulldown = 1b'0 */
	USBPHY_CLR8(0x6a, 0x20);
	/* 4 13. force_xcversel = 1b'0 */
	USBPHY_CLR8(0x6a, 0x08);
	/* 4 14. force_termsel = 1b'0*/
	USBPHY_CLR8(0x6a, 0x02);
	/* 4 15. force_datain = 1b'0*/
	USBPHY_CLR8(0x6a, 0x80);

	/* RG_USB20_BC11_SW_EN, 1'b0 */
	USBPHY_CLR8(0x1a, 0x80);
	/* RG_USB20_OTG_VBUSCMP_EN, 1'b1 */
	USBPHY_SET8(0x1a, 0x10);
	/*18. PASS RX sensitivity HQA requirement*/
	USBPHY_CLR8(0x18, 0x08);
	USBPHY_SET8(0x18, 0x06);

	/* wait 800 usec. */
	udelay(800);

	/* force enter device mode, from K2, FIXME */
	USBPHY_CLR8(0x6c, 0x10);
	USBPHY_SET8(0x6c, 0x2E);
	USBPHY_SET8(0x6d, 0x3E);

	/* from K2, FIXME */
	#if defined(MTK_HDMI_SUPPORT)
	USBPHY_SET8(0x05, 0x05);
	USBPHY_SET8(0x05, 0x50);
	#endif
	hs_slew_rate_cal();

	DBG(0, "usb recovery success\n");
}

/* BC1.2 */
void Charger_Detect_Init(void)
{
	unsigned long flags;
	int do_lock;

	do_lock = 0;
	#ifdef CONFIG_MTK_MUSB_PORT0_LOWPOWER_MODE
	mt_usb_clock_prepare();
	#endif
	if (mtk_musb) {
		spin_lock_irqsave(&mtk_musb->lock, flags);
		do_lock = 1;
	} else {
		DBG(0, "mtk_musb is NULL\n");

	}
	/* turn on USB reference clock. */
	usb_enable_clock(true);
	/* wait 50 usec. */
	udelay(50);
	/* RG_USB20_BC11_SW_EN = 1'b1 */
	USBPHY_SET8(0x1a, 0x80);

	if (do_lock)
		spin_unlock_irqrestore(&mtk_musb->lock, flags);
	DBG(0, "Charger_Detect_Init\n");
}

void Charger_Detect_Release(void)
{
	unsigned long flags;
	int do_lock;

	do_lock = 0;
	if (mtk_musb) {
		spin_lock_irqsave(&mtk_musb->lock, flags);
		do_lock = 1;
	} else {
		DBG(0, "mtk_musb is NULL\n");
	}

	/* RG_USB20_BC11_SW_EN = 1'b0 */
	USBPHY_CLR8(0x1a, 0x80);
	udelay(1);
	/* 4 14. turn off internal 48Mhz PLL. */
	usb_enable_clock(false);

	if (do_lock)
		spin_unlock_irqrestore(&mtk_musb->lock, flags);
	#ifdef CONFIG_MTK_MUSB_PORT0_LOWPOWER_MODE
	mt_usb_clock_unprepare();
	#endif
	DBG(0, "Charger_Detect_Release\n");
}

void usb_phy_context_save(void)
{
#ifdef CONFIG_MTK_UART_USB_SWITCH
	in_uart_mode = usb_phy_check_in_uart_mode();
	WARNING("in_uart_mode:%d\n", in_uart_mode);
#endif
}

void usb_phy_context_restore(void)
{
#ifdef CONFIG_MTK_UART_USB_SWITCH
	if (in_uart_mode)
		usb_phy_switch_to_uart();
#endif

	/* If defined MUSB_ENABLE_BUS_SUSPEND, **NEVER** go the flow :usb_phy_savecurrent_internal */
	DBG(0, "+, is %s\n", mtk_musb->is_host ? "Host" : "Device");

	if (!mtk_musb->is_host) {
		usb_phy_savecurrent_internal();
	} else {
	#ifndef MUSB_ENABLE_BUS_SUSPEND
		usb_phy_savecurrent_internal();
	#endif
	}
}

#endif