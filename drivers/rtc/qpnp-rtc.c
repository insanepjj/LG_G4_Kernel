/* Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/rtc.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/of_device.h>
#include <linux/spmi.h>
#include <linux/spinlock.h>
#include <linux/spmi.h>
#include <linux/alarmtimer.h>

/* RTC/ALARM Register offsets */
#define REG_OFFSET_ALARM_RW	0x40
#define REG_OFFSET_ALARM_CTRL1	0x46
#define REG_OFFSET_ALARM_CTRL2	0x48
#define REG_OFFSET_RTC_WRITE	0x40
#define REG_OFFSET_RTC_CTRL	0x46
#define REG_OFFSET_RTC_READ	0x48
#define REG_OFFSET_PERP_SUBTYPE	0x05

/* RTC_CTRL register bit fields */
#define BIT_RTC_ENABLE		BIT(7)
#define BIT_RTC_ALARM_ENABLE	BIT(7)
#define BIT_RTC_ABORT_ENABLE	BIT(0)
#define BIT_RTC_ALARM_CLEAR	BIT(0)

/* RTC/ALARM peripheral subtype values */
#define RTC_PERPH_SUBTYPE       0x1
#define ALARM_PERPH_SUBTYPE     0x3

#define NUM_8_BIT_RTC_REGS	0x4

#define TO_SECS(arr)		(arr[0] | (arr[1] << 8) | (arr[2] << 16) | \
							(arr[3] << 24))

#ifdef CONFIG_LGE_RTC_FAKE_SECS
static unsigned long rtc_fake_secs;
#endif

/* Module parameter to control power-on-alarm */
#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
extern bool poweron_alarm;
#else
static bool poweron_alarm;
#endif
module_param(poweron_alarm, bool, 0644);
MODULE_PARM_DESC(poweron_alarm, "Enable/Disable power-on alarm");

/* rtc driver internal structure */
struct qpnp_rtc {
	u8  rtc_ctrl_reg;
	u8  alarm_ctrl_reg1;
	u16 rtc_base;
	u16 alarm_base;
	u32 rtc_write_enable;
	u32 rtc_alarm_powerup;
	int rtc_alarm_irq;
	struct device *rtc_dev;
	struct rtc_device *rtc;
	struct spmi_device *spmi;
	spinlock_t alarm_ctrl_lock;
};

#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
extern struct rtc_wkalrm g_poalarm;
static struct workqueue_struct*	poa_workq;
static struct delayed_work poa_read_alarm_info;
#endif

static int qpnp_read_wrapper(struct qpnp_rtc *rtc_dd, u8 *rtc_val,
			u16 base, int count)
{
	int rc;
	struct spmi_device *spmi = rtc_dd->spmi;

	rc = spmi_ext_register_readl(spmi->ctrl, spmi->sid, base, rtc_val,
					count);
	if (rc) {
		dev_err(rtc_dd->rtc_dev, "SPMI read failed\n");
		return rc;
	}
	return 0;
}

static int qpnp_write_wrapper(struct qpnp_rtc *rtc_dd, u8 *rtc_val,
			u16 base, int count)
{
	int rc;
	struct spmi_device *spmi = rtc_dd->spmi;

	rc = spmi_ext_register_writel(spmi->ctrl, spmi->sid, base, rtc_val,
					count);
	if (rc) {
		dev_err(rtc_dd->rtc_dev, "SPMI write failed\n");
		return rc;
	}

	return 0;
}

static int
qpnp_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	int rc;
	unsigned long secs, irq_flags;
	u8 value[4], reg = 0, alarm_enabled = 0, ctrl_reg;
	u8 rtc_disabled = 0, rtc_ctrl_reg;
	struct qpnp_rtc *rtc_dd = dev_get_drvdata(dev);

	rtc_tm_to_time(tm, &secs);

#ifdef CONFIG_LGE_RTC_FAKE_SECS
	secs -= rtc_fake_secs;
#endif

	value[0] = secs & 0xFF;
	value[1] = (secs >> 8) & 0xFF;
	value[2] = (secs >> 16) & 0xFF;
	value[3] = (secs >> 24) & 0xFF;

	dev_dbg(dev, "Seconds value to be written to RTC = %lu\n", secs);

	spin_lock_irqsave(&rtc_dd->alarm_ctrl_lock, irq_flags);
	ctrl_reg = rtc_dd->alarm_ctrl_reg1;

	if (ctrl_reg & BIT_RTC_ALARM_ENABLE) {
		alarm_enabled = 1;
		ctrl_reg &= ~BIT_RTC_ALARM_ENABLE;
		rc = qpnp_write_wrapper(rtc_dd, &ctrl_reg,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
		if (rc) {
			dev_err(dev, "Write to ALARM ctrl reg failed\n");
			goto rtc_rw_fail;
		}
	} else
		spin_unlock_irqrestore(&rtc_dd->alarm_ctrl_lock, irq_flags);

	/*
	 * 32 bit seconds value is coverted to four 8 bit values
	 *	|<------  32 bit time value in seconds  ------>|
	 *      <- 8 bit ->|<- 8 bit ->|<- 8 bit ->|<- 8 bit ->|
	 *       ----------------------------------------------
	 *      | BYTE[3]  |  BYTE[2]  |  BYTE[1]  |  BYTE[0]  |
	 *       ----------------------------------------------
	 *
	 * RTC has four 8 bit registers for writting time in seconds:
	 *             WDATA[3], WDATA[2], WDATA[1], WDATA[0]
	 *
	 * Write to the RTC registers should be done in following order
	 * Clear WDATA[0] register
	 *
	 * Write BYTE[1], BYTE[2] and BYTE[3] of time to
	 * RTC WDATA[3], WDATA[2], WDATA[1] registers
	 *
	 * Write BYTE[0] of time to RTC WDATA[0] register
	 *
	 * Clearing BYTE[0] and writting in the end will prevent any
	 * unintentional overflow from WDATA[0] to higher bytes during the
	 * write operation
	 */

	/* Disable RTC H/w before writing on RTC register*/
	rtc_ctrl_reg = rtc_dd->rtc_ctrl_reg;
	if (rtc_ctrl_reg & BIT_RTC_ENABLE) {
		rtc_disabled = 1;
		rtc_ctrl_reg &= ~BIT_RTC_ENABLE;
		rc = qpnp_write_wrapper(rtc_dd, &rtc_ctrl_reg,
				rtc_dd->rtc_base + REG_OFFSET_RTC_CTRL, 1);
		if (rc) {
			dev_err(dev,
				"Disabling of RTC control reg failed"
					" with error:%d\n", rc);
			goto rtc_rw_fail;
		}
		rtc_dd->rtc_ctrl_reg = rtc_ctrl_reg;
	}

	/* Clear WDATA[0] */
	reg = 0x0;
	rc = qpnp_write_wrapper(rtc_dd, &reg,
				rtc_dd->rtc_base + REG_OFFSET_RTC_WRITE, 1);
	if (rc) {
		dev_err(dev, "Write to RTC reg failed\n");
		goto rtc_rw_fail;
	}

	/* Write to WDATA[3], WDATA[2] and WDATA[1] */
	rc = qpnp_write_wrapper(rtc_dd, &value[1],
			rtc_dd->rtc_base + REG_OFFSET_RTC_WRITE + 1, 3);
	if (rc) {
		dev_err(dev, "Write to RTC reg failed\n");
		goto rtc_rw_fail;
	}

	/* Write to WDATA[0] */
	rc = qpnp_write_wrapper(rtc_dd, value,
				rtc_dd->rtc_base + REG_OFFSET_RTC_WRITE, 1);
	if (rc) {
		dev_err(dev, "Write to RTC reg failed\n");
		goto rtc_rw_fail;
	}

	/* Enable RTC H/w after writing on RTC register*/
	if (rtc_disabled) {
		rtc_ctrl_reg |= BIT_RTC_ENABLE;
		rc = qpnp_write_wrapper(rtc_dd, &rtc_ctrl_reg,
				rtc_dd->rtc_base + REG_OFFSET_RTC_CTRL, 1);
		if (rc) {
			dev_err(dev,
				"Enabling of RTC control reg failed"
					" with error:%d\n", rc);
			goto rtc_rw_fail;
		}
		rtc_dd->rtc_ctrl_reg = rtc_ctrl_reg;
	}

	if (alarm_enabled) {
		ctrl_reg |= BIT_RTC_ALARM_ENABLE;
		rc = qpnp_write_wrapper(rtc_dd, &ctrl_reg,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
		if (rc) {
			dev_err(dev, "Write to ALARM ctrl reg failed\n");
			goto rtc_rw_fail;
		}
	}

	rtc_dd->alarm_ctrl_reg1 = ctrl_reg;

rtc_rw_fail:
	if (alarm_enabled)
		spin_unlock_irqrestore(&rtc_dd->alarm_ctrl_lock, irq_flags);

	return rc;
}

static int
qpnp_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	int rc;
	u8 value[4], reg;
	unsigned long secs;
	struct qpnp_rtc *rtc_dd = dev_get_drvdata(dev);

	rc = qpnp_read_wrapper(rtc_dd, value,
				rtc_dd->rtc_base + REG_OFFSET_RTC_READ,
				NUM_8_BIT_RTC_REGS);
	if (rc) {
		dev_err(dev, "Read from RTC reg failed\n");
		return rc;
	}

	/*
	 * Read the LSB again and check if there has been a carry over
	 * If there is, redo the read operation
	 */
	rc = qpnp_read_wrapper(rtc_dd, &reg,
				rtc_dd->rtc_base + REG_OFFSET_RTC_READ, 1);
	if (rc) {
		dev_err(dev, "Read from RTC reg failed\n");
		return rc;
	}

	if (reg < value[0]) {
		rc = qpnp_read_wrapper(rtc_dd, value,
				rtc_dd->rtc_base + REG_OFFSET_RTC_READ,
				NUM_8_BIT_RTC_REGS);
		if (rc) {
			dev_err(dev, "Read from RTC reg failed\n");
			return rc;
		}
	}

#ifdef CONFIG_LGE_RTC_FAKE_SECS
	secs = rtc_fake_secs + TO_SECS(value);
#else
	secs = TO_SECS(value);
#endif

	rtc_time_to_tm(secs, tm);

	rc = rtc_valid_tm(tm);
	if (rc) {
		dev_err(dev, "Invalid time read from RTC\n");
		return rc;
	}

	dev_dbg(dev, "secs = %lu, h:m:s == %d:%d:%d, d/m/y = %d/%d/%d\n",
			secs, tm->tm_hour, tm->tm_min, tm->tm_sec,
			tm->tm_mday, tm->tm_mon, tm->tm_year);

	return 0;
}

static int
qpnp_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	int rc;
	u8 value[4], ctrl_reg;
	unsigned long secs, secs_rtc, irq_flags;
	struct qpnp_rtc *rtc_dd = dev_get_drvdata(dev);
	struct rtc_time rtc_tm;

	rtc_tm_to_time(&alarm->time, &secs);

	/*
	 * Read the current RTC time and verify if the alarm time is in the
	 * past. If yes, return invalid
	 */
	rc = qpnp_rtc_read_time(dev, &rtc_tm);
	if (rc) {
		dev_err(dev, "Unable to read RTC time\n");
		return -EINVAL;
	}

	rtc_tm_to_time(&rtc_tm, &secs_rtc);
	if (secs < secs_rtc) {
		dev_err(dev, "Trying to set alarm in the past\n");
		return -EINVAL;
	}

#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
	if (g_poalarm.enabled) {
		unsigned long secs_pwron;
		/* If there are power on alarm before alarm time, ignore alarm */
		rtc_tm_to_time(&g_poalarm.time, &secs_pwron);
		pr_info("[%s %d] secs_pwron=%lu, secs=%lu, rtc=%lu\n",
					__func__, __LINE__, secs_pwron, secs, secs_rtc);

        if (secs_rtc < secs_pwron && secs_pwron < secs) {
            pr_info("[%s %d] Override with Power Off Alarm\n",
						__func__, __LINE__);
            memcpy(alarm, &g_poalarm, sizeof(struct rtc_wkalrm));
            secs = secs_pwron;
        }

		if (secs_pwron < secs_rtc) {
			pr_info("[%s %d] Power Off alarm was expired.\n",
						__func__, __LINE__);
			g_poalarm.enabled = 0;
			return 0;
		}
	}
#endif

#ifdef CONFIG_LGE_RTC_FAKE_SECS
	secs -= rtc_fake_secs;
#endif

	value[0] = secs & 0xFF;
	value[1] = (secs >> 8) & 0xFF;
	value[2] = (secs >> 16) & 0xFF;
	value[3] = (secs >> 24) & 0xFF;

	spin_lock_irqsave(&rtc_dd->alarm_ctrl_lock, irq_flags);

	rc = qpnp_write_wrapper(rtc_dd, value,
				rtc_dd->alarm_base + REG_OFFSET_ALARM_RW,
				NUM_8_BIT_RTC_REGS);
	if (rc) {
		dev_err(dev, "Write to ALARM reg failed\n");
		goto rtc_rw_fail;
	}

	ctrl_reg = (alarm->enabled) ?
			(rtc_dd->alarm_ctrl_reg1 | BIT_RTC_ALARM_ENABLE) :
			(rtc_dd->alarm_ctrl_reg1 & ~BIT_RTC_ALARM_ENABLE);

	rc = qpnp_write_wrapper(rtc_dd, &ctrl_reg,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
	if (rc) {
		dev_err(dev, "Write to ALARM cntrol reg failed\n");
		goto rtc_rw_fail;
	}

	rtc_dd->alarm_ctrl_reg1 = ctrl_reg;

	dev_dbg(dev, "Alarm Set for h:r:s=%d:%d:%d, d/m/y=%d/%d/%d\n",
			alarm->time.tm_hour, alarm->time.tm_min,
			alarm->time.tm_sec, alarm->time.tm_mday,
			alarm->time.tm_mon, alarm->time.tm_year);
rtc_rw_fail:
	spin_unlock_irqrestore(&rtc_dd->alarm_ctrl_lock, irq_flags);
	return rc;
}

static int
qpnp_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	int rc;
	u8 value[4];
	unsigned long secs;
	struct qpnp_rtc *rtc_dd = dev_get_drvdata(dev);

#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
	u8 ctrl_reg;
#endif

	rc = qpnp_read_wrapper(rtc_dd, value,
				rtc_dd->alarm_base + REG_OFFSET_ALARM_RW,
				NUM_8_BIT_RTC_REGS);
	if (rc) {
		dev_err(dev, "Read from ALARM reg failed\n");
		return rc;
	}

#ifdef CONFIG_LGE_RTC_FAKE_SECS
	secs = rtc_fake_secs + TO_SECS(value);
#else
	secs = TO_SECS(value);
#endif

#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
	ctrl_reg = rtc_dd->alarm_ctrl_reg1;
	if (ctrl_reg & BIT_RTC_ALARM_ENABLE) {
		alarm->enabled = true;
	} else {
		alarm->enabled = false;
	}
#endif

	rtc_time_to_tm(secs, &alarm->time);

	rc = rtc_valid_tm(&alarm->time);
	if (rc) {
		dev_err(dev, "Invalid time read from RTC\n");
		return rc;
	}

	dev_dbg(dev, "Alarm set for - h:r:s=%d:%d:%d, d/m/y=%d/%d/%d\n",
		alarm->time.tm_hour, alarm->time.tm_min,
				alarm->time.tm_sec, alarm->time.tm_mday,
				alarm->time.tm_mon, alarm->time.tm_year);

	return 0;
}


static int
qpnp_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	int rc;
	unsigned long irq_flags;
	struct qpnp_rtc *rtc_dd = dev_get_drvdata(dev);
	u8 ctrl_reg;
	u8 value[4] = {0};

#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
	pr_info("[%s]: enabled : %d, poweron_alarm : %d\n",
				__func__, enabled, poweron_alarm);
	if (poweron_alarm == true) {
		enabled = true;
	}
	else if (poweron_alarm == false) {
		enabled = false;
	}
#endif

	spin_lock_irqsave(&rtc_dd->alarm_ctrl_lock, irq_flags);
	ctrl_reg = rtc_dd->alarm_ctrl_reg1;
	ctrl_reg = enabled ? (ctrl_reg | BIT_RTC_ALARM_ENABLE) :
				(ctrl_reg & ~BIT_RTC_ALARM_ENABLE);

	rc = qpnp_write_wrapper(rtc_dd, &ctrl_reg,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
	if (rc) {
		dev_err(dev, "Write to ALARM control reg failed\n");
		goto rtc_rw_fail;
	}

	rtc_dd->alarm_ctrl_reg1 = ctrl_reg;

	/* Clear Alarm register */
	if (!enabled) {
		rc = qpnp_write_wrapper(rtc_dd, value,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_RW,
			NUM_8_BIT_RTC_REGS);
		if (rc)
			dev_err(dev, "Clear ALARM value reg failed\n");
	}

rtc_rw_fail:
	spin_unlock_irqrestore(&rtc_dd->alarm_ctrl_lock, irq_flags);
	return rc;
}

#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
static int
qpnp_rtc_alarm_proc(struct device *dev, struct seq_file *seq)
{
	int rc;
	unsigned long irq_flags;
	struct qpnp_rtc *rtc_dd = dev_get_drvdata(dev);
	u8 ctrl_reg;

	spin_lock_irqsave(&rtc_dd->alarm_ctrl_lock, irq_flags);
	ctrl_reg = rtc_dd->alarm_ctrl_reg1;
	if (ctrl_reg & BIT_RTC_ALARM_ENABLE) {
		rc = 1;
	}
	else {
		rc = 0;
	}
	spin_unlock_irqrestore(&rtc_dd->alarm_ctrl_lock, irq_flags);
	seq_printf(seq, "rtc_alarm_state\t: %s\n",
		     (rc) ? "yes" : "no");

	return 0;
}

static int
qpnp_rtc_set_po_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	int rc;
	u8 value[4], ctrl_reg;
	unsigned long secs, secs_rtc,irq_flags;
	struct qpnp_rtc *rtc_dd = dev_get_drvdata(dev);
	struct rtc_time rtc_tm;
	u8 pon_trigger_rtc;

	pon_trigger_rtc = 0xFE;
	pr_info("[%s] : alarm->enabled = %d, poweron_alarm = %d\n",
				__func__,alarm->enabled, poweron_alarm);

	if (!alarm->enabled) {

		pr_info("[%s %d] try to clear\n", __func__, __LINE__);

		ctrl_reg = (rtc_dd->alarm_ctrl_reg1 & ~BIT_RTC_ALARM_ENABLE);

		spin_lock_irqsave(&rtc_dd->alarm_ctrl_lock, irq_flags);

		rc = qpnp_write_wrapper(rtc_dd, &ctrl_reg, rtc_dd->alarm_base
											+ REG_OFFSET_ALARM_CTRL1, 1);
		if (rc < 0) {
			pr_err("[%s %d] QPNP RTC write failed!\n", __func__, __LINE__);
			goto rtc_rw_fail;
		}
		rtc_dd->alarm_ctrl_reg1= ctrl_reg;
	}
	else {
		/*
		 * Read the current RTC time and verify if the alarm time is in the
		 * past. If yes, return invalid
		 */
		rc = qpnp_rtc_read_time(dev, &rtc_tm);
		if (rc) {
			dev_err(dev, "Unable to read RTC time\n");
			return -EINVAL;
		}

		pr_info("[%s] RTC TIME %d-%02d-%02d %02d:%02d:%02d\n", __func__,
				rtc_tm.tm_year + 1900, rtc_tm.tm_mon + 1, rtc_tm.tm_mday,
				rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_sec);
		pr_info("[%s] ALARM TIME %d-%02d-%02d %02d:%02d:%02d\n", __func__,
				alarm->time.tm_year + 1900, alarm->time.tm_mon + 1, alarm->time.tm_mday,
				alarm->time.tm_hour, alarm->time.tm_min, alarm->time.tm_sec);

		rtc_tm_to_time(&rtc_tm, &secs_rtc);
		rtc_tm_to_time(&alarm->time, &secs);

		pr_info("[%s] rtc time = %lu, alarm time = %lu\n", __func__,
					secs_rtc, secs);

		if (secs < secs_rtc) {
			g_poalarm.enabled = 0;
			write_rtc_pwron_in_misc(&g_poalarm);
			dev_err(dev, "Trying to set alarm in the past\n");
			return -EINVAL;
		}
#ifdef CONFIG_LGE_RTC_FAKE_SECS
		secs -= rtc_fake_secs;
#endif
		value[0] = secs & 0xFF;
		value[1] = (secs >> 8) & 0xFF;
		value[2] = (secs >> 16) & 0xFF;
		value[3] = (secs >> 24) & 0xFF;

		spin_lock_irqsave(&rtc_dd->alarm_ctrl_lock, irq_flags);

		rc = qpnp_write_wrapper(rtc_dd, value,
				rtc_dd->alarm_base + REG_OFFSET_ALARM_RW,
				NUM_8_BIT_RTC_REGS);
		if (rc) {
			dev_err(dev, "Write to ALARM reg failed\n");
			goto rtc_rw_fail;
		}

		/* Enable PON Trigger for RTC */
		rc = qpnp_write_wrapper(rtc_dd, &pon_trigger_rtc, 0x880,
				NUM_8_BIT_RTC_REGS);
		if (rc) {
			dev_err(dev, "Write to ALARM reg failed\n");
			goto rtc_rw_fail;
		}

		ctrl_reg = (alarm->enabled) ?
				(rtc_dd->alarm_ctrl_reg1 | BIT_RTC_ALARM_ENABLE) :
				(rtc_dd->alarm_ctrl_reg1 & ~BIT_RTC_ALARM_ENABLE);

		rc = qpnp_write_wrapper(rtc_dd, &ctrl_reg,
				rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
		if (rc) {
			dev_err(dev, "Write to ALARM cntrol reg failed\n");
			goto rtc_rw_fail;
		}
		rc = qpnp_read_wrapper(rtc_dd, &ctrl_reg,
				rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
		if (rc) {
			dev_err(dev, "Write to ALARM cntrol reg failed\n");
			goto rtc_rw_fail;
		}
		rtc_dd->alarm_ctrl_reg1 = ctrl_reg;
		pr_info("[%s] ctrl_reg = 0x%x\n",__func__, ctrl_reg);
	}
rtc_rw_fail:
	spin_unlock_irqrestore(&rtc_dd->alarm_ctrl_lock, irq_flags);
	return rc;
}

static int qpnp_rtc_reset_po_alarm(struct device *dev)
{
	pr_info("[%s %d] enable=%d\n", __func__, __LINE__, g_poalarm.enabled);
	return qpnp_rtc_set_po_alarm(dev, &g_poalarm);
}

static void read_poa_info(struct work_struct *work)
{
	pr_info("[%s]\n",__func__);
}
#endif

static struct rtc_class_ops qpnp_rtc_ops = {
	.read_time = qpnp_rtc_read_time,
	.set_alarm = qpnp_rtc_set_alarm,
	.read_alarm = qpnp_rtc_read_alarm,
	.alarm_irq_enable = qpnp_rtc_alarm_irq_enable,
#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
	.proc 		= qpnp_rtc_alarm_proc,
	.set_po_alarm	= qpnp_rtc_set_po_alarm,
#endif
};

static irqreturn_t qpnp_alarm_trigger(int irq, void *dev_id)
{
	struct qpnp_rtc *rtc_dd = dev_id;
	u8 ctrl_reg;
	int rc;
	unsigned long irq_flags;

	rtc_update_irq(rtc_dd->rtc, 1, RTC_IRQF | RTC_AF);

	spin_lock_irqsave(&rtc_dd->alarm_ctrl_lock, irq_flags);

	/* Clear the alarm enable bit */
	ctrl_reg = rtc_dd->alarm_ctrl_reg1;
	ctrl_reg &= ~BIT_RTC_ALARM_ENABLE;

	rc = qpnp_write_wrapper(rtc_dd, &ctrl_reg,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
	if (rc) {
		spin_unlock_irqrestore(&rtc_dd->alarm_ctrl_lock, irq_flags);
		dev_err(rtc_dd->rtc_dev,
				"Write to ALARM control reg failed\n");
		goto rtc_alarm_handled;
	}

	rtc_dd->alarm_ctrl_reg1 = ctrl_reg;
	spin_unlock_irqrestore(&rtc_dd->alarm_ctrl_lock, irq_flags);

	/* Set ALARM_CLR bit */
	ctrl_reg = 0x1;
	rc = qpnp_write_wrapper(rtc_dd, &ctrl_reg,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL2, 1);
	if (rc)
		dev_err(rtc_dd->rtc_dev,
				"Write to ALARM control reg failed\n");

rtc_alarm_handled:
	return IRQ_HANDLED;
}

static int qpnp_rtc_probe(struct spmi_device *spmi)
{
	int rc;
	u8 subtype;
	struct qpnp_rtc *rtc_dd;
	struct resource *resource;
	struct spmi_resource *spmi_resource;

#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
	u8 ctrl_reg;
#endif

#ifdef CONFIG_LGE_RTC_FAKE_SECS
	rtc_fake_secs = mktime(2015, 1, 1, 0, 0, 0);
#endif

	rtc_dd = devm_kzalloc(&spmi->dev, sizeof(*rtc_dd), GFP_KERNEL);
	if (rtc_dd == NULL) {
		dev_err(&spmi->dev, "Unable to allocate memory!\n");
		return -ENOMEM;
	}

	/* Get the rtc write property */
	rc = of_property_read_u32(spmi->dev.of_node, "qcom,qpnp-rtc-write",
						&rtc_dd->rtc_write_enable);
	if (rc && rc != -EINVAL) {
		dev_err(&spmi->dev,
			"Error reading rtc_write_enable property %d\n", rc);
		return rc;
	}

	rc = of_property_read_u32(spmi->dev.of_node,
						"qcom,qpnp-rtc-alarm-pwrup",
						&rtc_dd->rtc_alarm_powerup);
	if (rc && rc != -EINVAL) {
		dev_err(&spmi->dev,
			"Error reading rtc_alarm_powerup property %d\n", rc);
		return rc;
	}

	/* Initialise spinlock to protect RTC control register */
	spin_lock_init(&rtc_dd->alarm_ctrl_lock);

	rtc_dd->rtc_dev = &(spmi->dev);
	rtc_dd->spmi = spmi;

	/* Get RTC/ALARM resources */
	spmi_for_each_container_dev(spmi_resource, spmi) {
		if (!spmi_resource) {
			dev_err(&spmi->dev,
				"%s: rtc_alarm: spmi resource absent!\n",
				__func__);
			rc = -ENXIO;
			goto fail_rtc_enable;
		}

		resource = spmi_get_resource(spmi, spmi_resource,
							IORESOURCE_MEM, 0);
		if (!(resource && resource->start)) {
			dev_err(&spmi->dev,
				"%s: node %s IO resource absent!\n",
				__func__, spmi->dev.of_node->full_name);
			rc = -ENXIO;
			goto fail_rtc_enable;
		}

		rc = qpnp_read_wrapper(rtc_dd, &subtype,
				resource->start + REG_OFFSET_PERP_SUBTYPE, 1);
		if (rc) {
			dev_err(&spmi->dev,
				"Peripheral subtype read failed\n");
			goto fail_rtc_enable;
		}

		switch (subtype) {
		case RTC_PERPH_SUBTYPE:
			rtc_dd->rtc_base = resource->start;
			break;
		case ALARM_PERPH_SUBTYPE:
			rtc_dd->alarm_base = resource->start;
			rtc_dd->rtc_alarm_irq =
				spmi_get_irq(spmi, spmi_resource, 0);
			if (rtc_dd->rtc_alarm_irq < 0) {
				dev_err(&spmi->dev, "ALARM IRQ absent\n");
				rc = -ENXIO;
				goto fail_rtc_enable;
			}
			break;
		default:
			dev_err(&spmi->dev, "Invalid peripheral subtype\n");
			rc = -EINVAL;
			goto fail_rtc_enable;
		}
	}

	rc = qpnp_read_wrapper(rtc_dd, &rtc_dd->rtc_ctrl_reg,
				rtc_dd->rtc_base + REG_OFFSET_RTC_CTRL, 1);
	if (rc) {
		dev_err(&spmi->dev,
			"Read from RTC control reg failed\n");
		goto fail_rtc_enable;
	}

	if (!(rtc_dd->rtc_ctrl_reg & BIT_RTC_ENABLE)) {
		dev_err(&spmi->dev,
			"RTC h/w disabled, rtc not registered\n");
		goto fail_rtc_enable;
	}

	rc = qpnp_read_wrapper(rtc_dd, &rtc_dd->alarm_ctrl_reg1,
				rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
	if (rc) {
		dev_err(&spmi->dev,
			"Read from  Alarm control reg failed\n");
		goto fail_rtc_enable;
	}
	/* Enable abort enable feature */
	rtc_dd->alarm_ctrl_reg1 |= BIT_RTC_ABORT_ENABLE;
	rc = qpnp_write_wrapper(rtc_dd, &rtc_dd->alarm_ctrl_reg1,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
	if (rc) {
		dev_err(&spmi->dev, "SPMI write failed!\n");
		goto fail_rtc_enable;
	}

	if (rtc_dd->rtc_write_enable == true)
		qpnp_rtc_ops.set_time = qpnp_rtc_set_time;

	dev_set_drvdata(&spmi->dev, rtc_dd);

	/* Register the RTC device */
	rtc_dd->rtc = rtc_device_register("qpnp_rtc", &spmi->dev,
						&qpnp_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc_dd->rtc)) {
		dev_err(&spmi->dev, "%s: RTC registration failed (%ld)\n",
					__func__, PTR_ERR(rtc_dd->rtc));
		rc = PTR_ERR(rtc_dd->rtc);
		goto fail_rtc_enable;
	}

	/* Init power_on_alarm after adding rtc device */
	power_on_alarm_init();

	/* Request the alarm IRQ */
	rc = request_any_context_irq(rtc_dd->rtc_alarm_irq,
				 qpnp_alarm_trigger, IRQF_TRIGGER_RISING,
				 "qpnp_rtc_alarm", rtc_dd);
	if (rc) {
		dev_err(&spmi->dev, "Request IRQ failed (%d)\n", rc);
		goto fail_req_irq;
	}

#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
	/* Read the ALARM EN Register */
	rc = qpnp_read_wrapper(rtc_dd, &ctrl_reg,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
	if (rc) {
		dev_err(rtc_dd->rtc_dev,"[%s %d] QPNP RTC read failed!\n",
					__func__,__LINE__);
	}
	pr_info("[%s %d] %x reg : value = %d\n",__func__, __LINE__,
				rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, ctrl_reg);

	if ((ctrl_reg >> 7) & 0x01) {
		poweron_alarm = 1;
	}
	else {
		poweron_alarm = 0;
	}
	pr_info("[%s %d] poweron_alarm = %d\n",__func__, __LINE__, poweron_alarm);

	poa_workq = create_singlethread_workqueue("pwroff_alarm");
	if (poa_workq == NULL) {
		dev_err(&spmi->dev, "pwroff_alarm work creating failed (%d)\n", rc);
	}

	INIT_DELAYED_WORK(&poa_read_alarm_info, read_poa_info);
	queue_delayed_work(poa_workq, &poa_read_alarm_info, 10*HZ);	// 10 seconds
#endif

	device_init_wakeup(&spmi->dev, 1);
	enable_irq_wake(rtc_dd->rtc_alarm_irq);

	dev_dbg(&spmi->dev, "Probe success !!\n");

	return 0;

fail_req_irq:
	rtc_device_unregister(rtc_dd->rtc);
fail_rtc_enable:
	dev_set_drvdata(&spmi->dev, NULL);

	return rc;
}

static int qpnp_rtc_remove(struct spmi_device *spmi)
{
	struct qpnp_rtc *rtc_dd = dev_get_drvdata(&spmi->dev);

#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
	destroy_workqueue(poa_workq);
#endif

	device_init_wakeup(&spmi->dev, 0);
	free_irq(rtc_dd->rtc_alarm_irq, rtc_dd);
	rtc_device_unregister(rtc_dd->rtc);
	dev_set_drvdata(&spmi->dev, NULL);

	return 0;
}
#ifdef CONFIG_LGE_PM_RTC_PWROFF_ALARM
static void qpnp_rtc_shutdown(struct spmi_device *spmi)
{
	u8 value[4] = {0, 0, 0, 0};
	u8 ctrl_reg;
	int rc;
	unsigned long secs;
	struct qpnp_rtc *rtc_dd = dev_get_drvdata(&spmi->dev);

	qpnp_rtc_reset_po_alarm(&spmi->dev);

	rc = qpnp_read_wrapper(rtc_dd, &ctrl_reg,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
	if(rc) {
		dev_err(rtc_dd->rtc_dev,"[%s %d] QPNP RTC read failed!\n",
					__func__,__LINE__);
	}

	rc = qpnp_read_wrapper(rtc_dd, value,
				rtc_dd->alarm_base + REG_OFFSET_ALARM_RW,
				NUM_8_BIT_RTC_REGS);
	if(rc) {
		dev_err(rtc_dd->rtc_dev,"[%s %d] QPNP RTC read failed!\n",
					__func__,__LINE__);
	}

	secs = TO_SECS(value);

	pr_info("[%s %d] secs = %lu\n", __func__, __LINE__, secs);
	pr_info("[%s %d] RTC Register : %d \n", __func__, __LINE__, ctrl_reg);
}
#else
static void qpnp_rtc_shutdown(struct spmi_device *spmi)
{
	u8 value[4] = {0};
	u8 reg;
	int rc;
	unsigned long irq_flags;
	struct qpnp_rtc *rtc_dd;
	bool rtc_alarm_powerup;

	if (!spmi) {
		pr_err("qpnp-rtc: spmi device not found\n");
		return;
	}
	rtc_dd = dev_get_drvdata(&spmi->dev);
	if (!rtc_dd) {
		pr_err("qpnp-rtc: rtc driver data not found\n");
		return;
	}
	rtc_alarm_powerup = rtc_dd->rtc_alarm_powerup;
	if (!rtc_alarm_powerup && !poweron_alarm) {
		spin_lock_irqsave(&rtc_dd->alarm_ctrl_lock, irq_flags);
		dev_dbg(&spmi->dev, "Disabling alarm interrupts\n");

		/* Disable RTC alarms */
		reg = rtc_dd->alarm_ctrl_reg1;
		reg &= ~BIT_RTC_ALARM_ENABLE;
		rc = qpnp_write_wrapper(rtc_dd, &reg,
			rtc_dd->alarm_base + REG_OFFSET_ALARM_CTRL1, 1);
		if (rc) {
			dev_err(rtc_dd->rtc_dev, "SPMI write failed\n");
			goto fail_alarm_disable;
		}

		/* Clear Alarm register */
		rc = qpnp_write_wrapper(rtc_dd, value,
				rtc_dd->alarm_base + REG_OFFSET_ALARM_RW,
				NUM_8_BIT_RTC_REGS);
		if (rc)
			dev_err(rtc_dd->rtc_dev, "SPMI write failed\n");

fail_alarm_disable:
		spin_unlock_irqrestore(&rtc_dd->alarm_ctrl_lock, irq_flags);
	}
}
#endif

static struct of_device_id spmi_match_table[] = {
	{
		.compatible = "qcom,qpnp-rtc",
	},
	{}
};

static struct spmi_driver qpnp_rtc_driver = {
	.probe          = qpnp_rtc_probe,
	.remove         = qpnp_rtc_remove,
	.shutdown       = qpnp_rtc_shutdown,
	.driver = {
		.name   = "qcom,qpnp-rtc",
		.owner  = THIS_MODULE,
		.of_match_table = spmi_match_table,
	},
};

static int __init qpnp_rtc_init(void)
{
	return spmi_driver_register(&qpnp_rtc_driver);
}
module_init(qpnp_rtc_init);

static void __exit qpnp_rtc_exit(void)
{
	spmi_driver_unregister(&qpnp_rtc_driver);
}
module_exit(qpnp_rtc_exit);

MODULE_DESCRIPTION("SMPI PMIC RTC driver");
MODULE_LICENSE("GPL V2");
