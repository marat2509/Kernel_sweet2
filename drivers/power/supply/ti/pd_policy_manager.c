
#define pr_fmt(fmt)	"[USBPD-PM]: %s: " fmt, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/power_supply.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/usb/usbpd.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/pmic-voter.h>

#include "pd_policy_manager.h"

#define PD_SRC_PDO_TYPE_FIXED		0
#define PD_SRC_PDO_TYPE_BATTERY		1
#define PD_SRC_PDO_TYPE_VARIABLE	2
#define PD_SRC_PDO_TYPE_AUGMENTED	3

#define BATT_MAX_CHG_VOLT		4400
#define BATT_FAST_CHG_CURR		12000
#define	BUS_OVP_THRESHOLD		12000
#define	BUS_OVP_ALARM_THRESHOLD		9500
#define	APDO_MAX_VOLT		12000

#define BUS_VOLT_INIT_UP		400

#define BAT_VOLT_LOOP_LMT		BATT_MAX_CHG_VOLT
#define BAT_CURR_LOOP_LMT		BATT_FAST_CHG_CURR
#define BUS_VOLT_LOOP_LMT		BUS_OVP_THRESHOLD

#define PM_WORK_RUN_INTERVAL		1000

enum {
	PM_ALGO_RET_OK,
	PM_ALGO_RET_THERM_FAULT,
	PM_ALGO_RET_OTHER_FAULT,
	PM_ALGO_RET_CHG_DISABLED,
	PM_ALGO_RET_TAPER_DONE,
	PM_ALGO_RET_SLOWLY_CHARGING,
};

enum {
	VBUS_ERROR_NONE,
	VBUS_ERROR_LOW,
	VBUS_ERROR_HIGH,
};

enum {
	SC8551_CHARGE_MODE_DIV2,
	SC8551_CHARGE_MODE_BYPASS,
};

static struct pdpm_config pm_config = {
	.bat_volt_lp_lmt		= BAT_VOLT_LOOP_LMT,
	.bat_curr_lp_lmt		= BAT_CURR_LOOP_LMT + 1000,
	.bus_volt_lp_lmt		= BUS_VOLT_LOOP_LMT,
	.bus_curr_lp_lmt		= BAT_CURR_LOOP_LMT >> 1,
	.bus_curr_compensate	= 0,

	.fc2_taper_current		= 2000,
	.fc2_steps			= 1,

	.min_adapter_volt_required	= 10000,
	.min_adapter_curr_required	= 2000,

	.min_vbat_for_cp		= 3500,

	.cp_sec_enable			= false,
	.fc2_disable_sw			= true,
};

static struct usbpd_pm *__pdpm;

static int fc2_taper_timer;
static int cool_overcharge_timer;
static int ibus_lmt_change_timer;


static void usbpd_check_usb_psy(struct usbpd_pm *pdpm)
{
	if (!pdpm->usb_psy) {
		pdpm->usb_psy = power_supply_get_by_name("usb");
		if (!pdpm->usb_psy)
			pr_err("usb psy not found!\n");
	}
}

static void usbpd_check_batt_psy(struct usbpd_pm *pdpm)
{
	if (!pdpm->sw_psy) {
		pdpm->sw_psy = power_supply_get_by_name("battery");
		if (!pdpm->sw_psy)
			pr_err("batt psy not found!\n");
	}
}

static void usbpd_check_bms_psy(struct usbpd_pm *pdpm)
{
	if (!pdpm->bms_psy) {
		pdpm->bms_psy = power_supply_get_by_name("bms");
		if (!pdpm->bms_psy)
			pr_err("bms psy not found!\n");
	}
}

static int pd_get_batt_current_thermal_level(struct usbpd_pm *pdpm, int *level)
{
	union power_supply_propval pval = {0,};
	int rc = 0;

	usbpd_check_batt_psy(pdpm);

	if (!pdpm->sw_psy)
		return -ENODEV;

	rc = power_supply_get_property(pdpm->sw_psy,
				POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT, &pval);
	if (rc < 0) {
		pr_info("Couldn't get fastcharge mode:%d\n", rc);
		return rc;
	}

	pr_debug("pval.intval: %d\n", pval.intval);

	*level = pval.intval;
	return rc;
}

/* get capacity from battery power supply property */
static int pd_get_batt_capacity(struct usbpd_pm *pdpm, int *capacity)
{
	union power_supply_propval pval = {0,};
	int rc = 0;

	usbpd_check_batt_psy(pdpm);

	if (!pdpm->sw_psy)
		return -ENODEV;

	rc = power_supply_get_property(pdpm->sw_psy,
				POWER_SUPPLY_PROP_CAPACITY, &pval);
	if (rc < 0) {
		pr_info("Couldn't get battery capacity:%d\n", rc);
		return rc;
	}

	pr_err("battery capacity is : %d\n", pval.intval);

	*capacity = pval.intval;
	return rc;
}

/* determine whether to disable cp according to jeita status */
static bool pd_disable_cp_by_jeita_status(struct usbpd_pm *pdpm)
{
	union power_supply_propval pval = {0,};
	int batt_temp = 0, bq_input_suspend = 0;
	int warm_thres, cool_thres;
	int rc;

	usbpd_check_batt_psy(pdpm);

	if (!pdpm->sw_psy)
		return -ENODEV;

	rc = power_supply_get_property(pdpm->sw_psy,
				POWER_SUPPLY_PROP_BQ_INPUT_SUSPEND, &pval);
	if (!rc)
		bq_input_suspend = !!pval.intval;

	pr_info("bq_input_suspend: %d\n", bq_input_suspend);

	if (bq_input_suspend)
		return true;

	if (!pdpm->bms_psy)
		return false;

	rc = power_supply_get_property(pdpm->bms_psy,
				POWER_SUPPLY_PROP_TEMP, &pval);
	if (rc < 0) {
		pr_info("Couldn't get batt temp prop:%d\n", rc);
		return false;
	}

	batt_temp = pval.intval;
	pr_info("batt_temp: %d\n", batt_temp);

	if (pdpm->cp.sc8551_charge_mode == SC8551_CHARGE_MODE_DIV2
			|| pdpm->cp.sc8551_charge_mode == SC8551_CHARGE_MODE_BYPASS) {
		warm_thres = JEITA_BYPASS_WARM_THR;
		cool_thres = JEITA_BYPASS_COOL_THR;
	} else {
		warm_thres = JEITA_WARM_THR;
		cool_thres = JEITA_COOL_THR;
	}
	pr_info("warm_thres: %d\n", warm_thres);
	pr_info("cool_thres: %d\n", cool_thres);

	if (batt_temp > warm_thres && !pdpm->jeita_triggered) {
		pdpm->jeita_triggered = true;
		return true;
	} else if (batt_temp < cool_thres && !pdpm->jeita_triggered) {
		pdpm->jeita_triggered = true;
		return true;
	} else if ((batt_temp <= (warm_thres - JEITA_HYSTERESIS))
			&& (batt_temp >= (cool_thres + JEITA_HYSTERESIS))
			&& pdpm->jeita_triggered) {
		pdpm->jeita_triggered = false;
		return false;
	} else {
		return pdpm->jeita_triggered;
	}

}

static bool is_cool_charge(struct usbpd_pm *pdpm)
{
	union power_supply_propval pval = {0,};
	int batt_temp = 0;
	int rc;

	if (!pdpm->bms_psy)
		return false;

	rc = power_supply_get_property(pdpm->bms_psy,
				POWER_SUPPLY_PROP_TEMP, &pval);
	if (rc < 0) {
		pr_info("Couldn't get batt temp prop:%d\n", rc);
		return false;
	}
	batt_temp = pval.intval;

	pr_debug("batt_temp: %d\n", batt_temp);
	if (batt_temp < 150)
		return true;
	return false;
}

static bool pd_get_bms_digest_verified(struct usbpd_pm *pdpm)
{
	union power_supply_propval pval = {0,};
	int rc;

	return true;

	if (!pdpm->bms_psy)
		return false;

	rc = power_supply_get_property(pdpm->bms_psy,
				POWER_SUPPLY_PROP_AUTHENTIC, &pval);
	if (rc < 0) {
		pr_info("Couldn't get fastcharge mode:%d\n", rc);
		return false;
	}

	pr_err("battery verify is: %d\n", pval.intval);

	if (pval.intval == 1)
		return true;
	else
		return false;
}

static void usbpd_check_cp_psy(struct usbpd_pm *pdpm)
{
	if (!pdpm->cp_psy) {
		if (pm_config.cp_sec_enable)
			pdpm->cp_psy = power_supply_get_by_name("bq2597x-master");
		else
			pdpm->cp_psy = power_supply_get_by_name("bq2597x-standalone");
		if (!pdpm->cp_psy)
		{
			pdpm->cp_psy = power_supply_get_by_name("ln8000");
			if (!pdpm->cp_psy)
				pr_err("cp_psy not found\n");
		}
	}
}

static void usbpd_check_cp_sec_psy(struct usbpd_pm *pdpm)
{
	if (!pdpm->cp_sec_psy) {
		pdpm->cp_sec_psy = power_supply_get_by_name("bq2597x-slave");
		if (!pdpm->cp_sec_psy)
			pr_err("cp_sec_psy not found\n");
	}
}

static int usbpd_get_effective_fcc_val(struct usbpd_pm *pdpm)
{
	int effective_fcc_val = 0;

	if (!pdpm->fcc_votable)
		pdpm->fcc_votable = find_votable("FCC");

	if (!pdpm->fcc_votable)
		return -EINVAL;

	effective_fcc_val = get_effective_result(pdpm->fcc_votable);
	effective_fcc_val = effective_fcc_val / 1000;
	pr_info("effective_fcc_val: %d\n", effective_fcc_val);
	return effective_fcc_val;
}

static int usbpd_config_max_vbat(struct usbpd_pm *pdpm)
{
	int ret;
	bool ffc_enable;
	union power_supply_propval val = {0,};

	usbpd_check_cp_psy(pdpm);
	usbpd_check_bms_psy(pdpm);

	if (!pdpm->cp_psy || !pdpm->bms_psy)
		return -ENODEV;

	ret = power_supply_get_property(pdpm->bms_psy, POWER_SUPPLY_PROP_FASTCHARGE_MODE, &val);
	if (ret)
		return ret;
	else
		ffc_enable = val.intval;

	if (ffc_enable)
		pm_config.bat_volt_lp_lmt = pdpm->ffc_bat_volt_max;
	else
		pm_config.bat_volt_lp_lmt = pdpm->bat_volt_max;

	pr_info("config max_vbat, ffc_enable = %d, max_vbat = %d", ffc_enable, pm_config.bat_volt_lp_lmt);
	return ret;
}

static void usbpd_pm_update_cp_status(struct usbpd_pm *pdpm)
{
	int ret;
	union power_supply_propval val = {0,};

	usbpd_check_cp_psy(pdpm);
	usbpd_check_bms_psy(pdpm);
	usbpd_check_usb_psy(pdpm);

	if (!pdpm->cp_psy || (!pdpm->bms_psy) || (!pdpm->usb_psy))
		return;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_CHARGE_MODE, &val);
	if (!ret)
		pdpm->cp.sc8551_charge_mode = val.intval;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_BYPASS_MODE_ENABLED, &val);
	if (!ret)
		pdpm->cp.sc8551_bypass_charge_enable = val.intval;

	ret = power_supply_get_property(pdpm->bms_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	if (!ret)
		pdpm->cp.vbat_volt = (val.intval / 1000);

	ret = power_supply_get_property(pdpm->usb_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	if (!ret)
		pdpm->cp.vbus_volt = val.intval/1000;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_BUS_CURRENT, &val);
	if (!ret)
		pdpm->cp.ibus_curr = val.intval;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_BUS_TEMPERATURE, &val);
	if (!ret)
		pdpm->cp.bus_temp = val.intval;

	ret = power_supply_get_property(pdpm->bms_psy,
			POWER_SUPPLY_PROP_TEMP, &val);
	if (!ret)
		pdpm->cp.bat_temp = val.intval;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_DIE_TEMPERATURE, &val);
	if (!ret)
		pdpm->cp.die_temp = val.intval;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_BATTERY_PRESENT, &val);
	if (!ret)
		pdpm->cp.batt_pres = val.intval;

	ret = power_supply_get_property(pdpm->usb_psy,
			POWER_SUPPLY_PROP_PRESENT, &val);
	if (!ret)
		pdpm->cp.vbus_pres = val.intval;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_BUS_ERROR_STATUS, &val);
	if (!ret)
		pdpm->cp.bus_error_status = val.intval;

	ret = power_supply_get_property(pdpm->bms_psy,
			POWER_SUPPLY_PROP_CURRENT_NOW, &val);
	if (!ret) {
		pdpm->cp.ibat_curr = (val.intval / 1000);
	}

	usbpd_config_max_vbat(pdpm);

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
	if (!ret)
		pdpm->cp.charge_enabled = val.intval;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_ALARM_STATUS, &val);
	if (!ret) {
		pdpm->cp.bat_ovp_alarm = !!(val.intval & BAT_OVP_ALARM_MASK);
		pdpm->cp.bat_ocp_alarm = !!(val.intval & BAT_OCP_ALARM_MASK);
		pdpm->cp.bus_ovp_alarm = !!(val.intval & BUS_OVP_ALARM_MASK);
		pdpm->cp.bus_ocp_alarm = !!(val.intval & BUS_OCP_ALARM_MASK);
		pdpm->cp.bat_ucp_alarm = !!(val.intval & BAT_UCP_ALARM_MASK);
		pdpm->cp.bat_therm_alarm = !!(val.intval & BAT_THERM_ALARM_MASK);
		pdpm->cp.bus_therm_alarm = !!(val.intval & BUS_THERM_ALARM_MASK);
		pdpm->cp.die_therm_alarm = !!(val.intval & DIE_THERM_ALARM_MASK);
	}

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_FAULT_STATUS, &val);
	if (!ret) {
		pdpm->cp.bat_ovp_fault = !!(val.intval & BAT_OVP_FAULT_MASK);
		pdpm->cp.bat_ocp_fault = !!(val.intval & BAT_OCP_FAULT_MASK);
		pdpm->cp.bus_ovp_fault = !!(val.intval & BUS_OVP_FAULT_MASK);
		pdpm->cp.bus_ocp_fault = !!(val.intval & BUS_OCP_FAULT_MASK);
		pdpm->cp.bat_therm_fault = !!(val.intval & BAT_THERM_FAULT_MASK);
		pdpm->cp.bus_therm_fault = !!(val.intval & BUS_THERM_FAULT_MASK);
		pdpm->cp.die_therm_fault = !!(val.intval & DIE_THERM_FAULT_MASK);
	}

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_REG_STATUS, &val);
	if (!ret) {
		pdpm->cp.vbat_reg = !!(val.intval & VBAT_REG_STATUS_MASK);
		pdpm->cp.ibat_reg = !!(val.intval & IBAT_REG_STATUS_MASK);
	}
}

static void usbpd_pm_update_cp_sec_status(struct usbpd_pm *pdpm)
{
	int ret;
	union power_supply_propval val = {0,};

	if (!pm_config.cp_sec_enable)
		return;

	usbpd_check_cp_sec_psy(pdpm);

	if (!pdpm->cp_sec_psy)
		return;

	ret = power_supply_get_property(pdpm->cp_sec_psy,
			POWER_SUPPLY_PROP_TI_BUS_CURRENT, &val);
	if (!ret)
		pdpm->cp_sec.ibus_curr = val.intval;

	ret = power_supply_get_property(pdpm->cp_sec_psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
	if (!ret)
		pdpm->cp_sec.charge_enabled = val.intval;
}

static int usbpd_pm_enable_cp(struct usbpd_pm *pdpm, bool enable)
{
	int ret;
	union power_supply_propval val = {0,};

	usbpd_check_cp_psy(pdpm);

	if (!pdpm->cp_psy)
		return -ENODEV;

	val.intval = enable;
	ret = power_supply_set_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);

	return ret;
}

static int usbpd_pm_enable_cp_sec(struct usbpd_pm *pdpm, bool enable)
{
	int ret;
	union power_supply_propval val = {0,};

	usbpd_check_cp_sec_psy(pdpm);

	if (!pdpm->cp_sec_psy)
		return -ENODEV;

	val.intval = enable;
	ret = power_supply_set_property(pdpm->cp_sec_psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);

	return ret;
}

static int usbpd_pm_check_cp_enabled(struct usbpd_pm *pdpm)
{
	int ret;
	union power_supply_propval val = {0,};

	usbpd_check_cp_psy(pdpm);

	if (!pdpm->cp_psy)
		return -ENODEV;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
	if (!ret)
		pdpm->cp.charge_enabled = !!val.intval;

	pr_info("pdpm->cp.charge_enabled:%d\n", pdpm->cp.charge_enabled);

	return ret;
}

static int usbpd_pm_check_cp_sec_enabled(struct usbpd_pm *pdpm)
{
	int ret;
	union power_supply_propval val = {0,};

	usbpd_check_cp_sec_psy(pdpm);

	if (!pdpm->cp_sec_psy)
		return -ENODEV;

	ret = power_supply_get_property(pdpm->cp_sec_psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
	if (!ret)
		pdpm->cp_sec.charge_enabled = !!val.intval;
	pr_info("pdpm->cp_sec.charge_enabled:%d\n", pdpm->cp_sec.charge_enabled);
	return ret;
}

static int usbpd_pm_set_cp_charge_mode(struct usbpd_pm *pdpm, int mode)
{
	int ret;
	union power_supply_propval val = {0,};

	usbpd_check_cp_psy(pdpm);
	if (!pdpm->cp_psy)
		return -ENODEV;

	if (mode != SC8551_CHARGE_MODE_DIV2 && mode != SC8551_CHARGE_MODE_BYPASS) {
		pr_err("%s, chg_mode(%d) is invalid.\n", __func__, mode);
		return -EINVAL;
	}

	pr_info("%s, chg_mode is set to %d\n", __func__, mode);
	val.intval = mode;
	ret = power_supply_set_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_CHARGE_MODE, &val);

	return ret;
}

static int usbpd_pm_update_cp_charge_mode(struct usbpd_pm *pdpm)
{
	int fcc = 0, chg_mode = 0;
	int thermal_level = 0;

	usbpd_check_bms_psy(pdpm);
	if (!pdpm->bms_psy)
		return -ENODEV;

	if (pdpm->cp.sc8551_bypass_charge_enable == 0) {
		chg_mode = SC8551_CHARGE_MODE_DIV2;
	} else {
		fcc = usbpd_get_effective_fcc_val(pdpm);
		pd_get_batt_current_thermal_level(pdpm, &thermal_level);
		if ((thermal_level > BYPASS_THERMAL_ENTER_LEVEL
				|| fcc <= BYPASS_FCC_ENTER_THRES)
				&& pdpm->cp.vbat_volt > BYPASS_VBAT_ENTER_THRES) {
			chg_mode = SC8551_CHARGE_MODE_BYPASS;
		} else if (thermal_level < BYPASS_THERMAL_ENTER_LEVEL
				|| fcc > BYPASS_FCC_EXIT_THRES){
			chg_mode = SC8551_CHARGE_MODE_DIV2;
		} else {
			chg_mode = pdpm->cp.sc8551_charge_mode;
		}
	}

	pr_info("bypass_en:%d, cp_chg_mode:%d:%d, therm_level:%d, fcc:%d, vbat:%d\n",
			pdpm->cp.sc8551_bypass_charge_enable,
			pdpm->cp.sc8551_charge_mode, chg_mode,
			thermal_level, fcc, pdpm->cp.vbat_volt);

	return chg_mode;
}

static int usbpd_pm_check_cp_charge_mode(struct usbpd_pm *pdpm)
{
	int ret = 0;
	union power_supply_propval val = {0,};

	usbpd_check_cp_psy(pdpm);
	if (!pdpm->cp_psy)
		return -ENODEV;

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_TI_CHARGE_MODE, &val);
	if (!ret)
		pdpm->cp.sc8551_charge_mode = val.intval;

	return ret;
}

static int usbpd_pm_switch_cp_charge_mode(struct usbpd_pm *pdpm)
{
	int ret = 0;
	int chg_mode = 0;

	ret = usbpd_pm_check_cp_charge_mode(pdpm);
	if (ret < 0) {
		pr_err("Failed to check cp charge mode, ret:%d.\n", ret);
		return ret;
	}

	if (pdpm->cp.sc8551_bypass_charge_enable != 1)
		return -EPERM;

	chg_mode = usbpd_pm_update_cp_charge_mode(pdpm);
	if (chg_mode != pdpm->cp.sc8551_charge_mode) {
		if (pdpm->cp.charge_enabled) {
			usbpd_pm_enable_cp(pdpm, false);
			msleep(30);
			usbpd_pm_check_cp_enabled(pdpm);
		}

		pr_info("cp charge mode switch from %d to %d.\n",
				pdpm->cp.sc8551_charge_mode, chg_mode);
		usbpd_pm_set_cp_charge_mode(pdpm, chg_mode);

		ret = usbpd_pm_check_cp_charge_mode(pdpm);
		if (ret < 0)
			pr_err("Failed to check cp charge mode, ret:%d.\n", ret);

		ret = 1;
	}

	return ret;
}

static int usbpd_pm_enable_sw(struct usbpd_pm *pdpm, bool enable)
{
	int ret;
	union power_supply_propval val = {0,};

	if (!pdpm->sw_psy) {
		pdpm->sw_psy = power_supply_get_by_name("battery");
		if (!pdpm->sw_psy) {
			return -ENODEV;
		}
	}

	val.intval = enable;
	ret = power_supply_set_property(pdpm->sw_psy,
			POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &val);

	return ret;
}

static int usbpd_pm_check_slowly_charging_enabled(struct usbpd_pm *pdpm)
{
	int ret;
	union power_supply_propval val = {0,};

	if (!pdpm->sw_psy) {
		pdpm->sw_psy = power_supply_get_by_name("battery");
		if (!pdpm->sw_psy) {
			return -ENODEV;
		}
	}

	ret = power_supply_get_property(pdpm->sw_psy,
			POWER_SUPPLY_PROP_SLOWLY_CHARGING, &val);
	if (!ret)
		pdpm->sw.slowly_charging = !!val.intval;

	return ret;
}

static int usbpd_pm_limit_sw(struct usbpd_pm *pdpm, bool enable)
{
	int ret;
	union power_supply_propval val = {0,};

	if (!pdpm->sw_psy) {
		pdpm->sw_psy = power_supply_get_by_name("battery");
		if (!pdpm->sw_psy) {
			return -ENODEV;
		}
	}

	val.intval = enable;
	ret = power_supply_set_property(pdpm->sw_psy,
			POWER_SUPPLY_PROP_BATTERY_CHARGING_LIMITED, &val);

	return ret;
}

static int usbpd_pm_check_sw_limited(struct usbpd_pm *pdpm)
{
	int ret;
	union power_supply_propval val = {0,};

	if (!pdpm->sw_psy) {
		pdpm->sw_psy = power_supply_get_by_name("battery");
		if (!pdpm->sw_psy) {
			return -ENODEV;
		}
	}

	ret = power_supply_get_property(pdpm->sw_psy,
			POWER_SUPPLY_PROP_BATTERY_CHARGING_LIMITED, &val);
	if (!ret)
		pdpm->sw.charge_limited = !!val.intval;

	return ret;
}

static int usbpd_pm_check_sw_enabled(struct usbpd_pm *pdpm)
{
	int ret;
	union power_supply_propval val = {0,};

	if (!pdpm->sw_psy) {
		pdpm->sw_psy = power_supply_get_by_name("battery");
		if (!pdpm->sw_psy) {
			return -ENODEV;
		}
	}

	ret = power_supply_get_property(pdpm->sw_psy,
			POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &val);
	if (!ret)
		pdpm->sw.charge_enabled = !!val.intval;

	return ret;
}

static void usbpd_pm_update_sw_status(struct usbpd_pm *pdpm)
{
	usbpd_pm_check_sw_enabled(pdpm);
	usbpd_pm_check_sw_limited(pdpm);
}

static void usbpd_pm_evaluate_src_caps(struct usbpd_pm *pdpm)
{
	int ret;
	int i;
	union power_supply_propval pval = {0, };
	int apdo_max = 0;

	if (!pdpm->pd) {
		pdpm->pd = smb_get_usbpd();
		if (!pdpm->pd) {
			pr_err("couldn't get usbpd device\n");
			return;
		}
	}

	ret = usbpd_fetch_pdo(pdpm->pd, pdpm->pdo);
	if (ret) {
		pr_err("Failed to fetch pdo info\n");
		return;
	}

	pdpm->apdo_max_volt = pm_config.min_adapter_volt_required;
	pdpm->apdo_max_curr = pm_config.min_adapter_curr_required;

	for (i = 0; i < PDO_MAX_NUM; i++) {
		if (pdpm->pdo[i].type == PD_SRC_PDO_TYPE_AUGMENTED
			&& pdpm->pdo[i].pps && pdpm->pdo[i].pos) {
			if (pdpm->pdo[i].max_volt_mv > 11000)
				pdpm->pdo[i].max_volt_mv = 11000;
			if (apdo_max < ((pdpm->pdo[i].max_volt_mv/1000) * (pdpm->pdo[i].curr_ma/10) / 100)) {
				apdo_max = (pdpm->pdo[i].max_volt_mv/1000) * (pdpm->pdo[i].curr_ma/10) / 100;
			}
			pr_err("%s max_volt:%d, curr_ma:%d, apdo_max:%d\n", __func__,
							pdpm->pdo[i].max_volt_mv, pdpm->pdo[i].curr_ma, apdo_max);
			if (pdpm->pdo[i].max_volt_mv >= pdpm->apdo_max_volt
					&& pdpm->pdo[i].curr_ma >= pdpm->apdo_max_curr
					&& pdpm->pdo[i].max_volt_mv <= APDO_MAX_VOLT) {
				pdpm->apdo_max_volt = pdpm->pdo[i].max_volt_mv;
				pdpm->apdo_max_curr = pdpm->pdo[i].curr_ma;
				pdpm->apdo_selected_pdo = pdpm->pdo[i].pos;
				pdpm->pps_supported = true;
			}
		}
	}

	if (pdpm->pps_supported) {
		pr_err("PPS supported, preferred APDO pos:%d, max volt:%d, current:%d\n",
				pdpm->apdo_selected_pdo,
				pdpm->apdo_max_volt,
				pdpm->apdo_max_curr);
		if (pdpm->apdo_max_curr <= LOW_POWER_PPS_CURR_THR)
			pdpm->apdo_max_curr = XIAOMI_LOW_POWER_PPS_CURR_MAX;
		pval.intval = (pdpm->apdo_max_volt / 1000) * (pdpm->apdo_max_curr / 10) / 100;
		if (pval.intval < apdo_max) {
			pval.intval = apdo_max;
		}
		power_supply_set_property(pdpm->usb_psy,
				POWER_SUPPLY_PROP_APDO_MAX, &pval);
	} else {
		pr_err("Not qualified PPS adapter\n");
	}
}

static void usbpd_pm_src_caps_update(struct usbpd_pm *pdpm)
{
	int ret;
	int i;

	if (!pdpm->pd) {
		pdpm->pd = smb_get_usbpd();
		if (!pdpm->pd) {
			pr_err("couldn't get usbpd device\n");
			return;
		}
	}

	ret = usbpd_fetch_pdo(pdpm->pd, pdpm->pdo);
	if (ret) {
		pr_err("Failed to fetch pdo info\n");
		return;
	}

	pdpm->apdo_max_volt = pm_config.min_adapter_volt_required;
	pdpm->apdo_max_curr = pm_config.min_adapter_curr_required;

	for (i = 0; i < PDO_MAX_NUM; i++) {
		if (pdpm->pdo[i].type == PD_SRC_PDO_TYPE_AUGMENTED
			&& pdpm->pdo[i].pps && pdpm->pdo[i].pos) {
			if (pdpm->pdo[i].max_volt_mv >= pdpm->apdo_max_volt
					&& pdpm->pdo[i].curr_ma >= pdpm->apdo_max_curr
					&& pdpm->pdo[i].max_volt_mv <= APDO_MAX_VOLT) {
				pdpm->apdo_max_volt = pdpm->pdo[i].max_volt_mv;
				pdpm->apdo_max_curr = pdpm->pdo[i].curr_ma;
				pdpm->apdo_selected_pdo = pdpm->pdo[i].pos;
				pdpm->pps_supported = true;
			}
		}
	}
	pr_err("%s max_volt:%d, curr_ma:%d, pdo_num:%d \n", __func__, pdpm->apdo_max_volt, pdpm->apdo_max_curr, i);
}

static void usbpd_update_pps_status(struct usbpd_pm *pdpm)
{
	int ret;
	u32 status;

	return;

	ret = usbpd_get_pps_status(pdpm->pd, &status);

	if (!ret) {
		pr_info("get_pps_status: status_db :0x%x\n", status);
		pdpm->adapter_voltage = (status & 0xFFFF) * 20;
		pdpm->adapter_current = ((status >> 16) & 0xFF) * 50;
		pdpm->adapter_ptf = ((status >> 24) & 0x06) >> 1;
		pdpm->adapter_omf = !!((status >> 24) & 0x08);
		pr_info("adapter_volt:%d, adapter_current:%d\n",
				pdpm->adapter_voltage, pdpm->adapter_current);
		pr_info("pdpm->adapter_ptf:%d, pdpm->adapter_omf:%d\n",
				pdpm->adapter_ptf, pdpm->adapter_omf);
	}
}

#define TAPER_TIMEOUT	(5000 / PM_WORK_RUN_INTERVAL)
#define IBUS_CHANGE_TIMEOUT  (500 / PM_WORK_RUN_INTERVAL)

static int usbpd_pm_fc2_charge_algo(struct usbpd_pm *pdpm)
{
	int ret = 0;
	int steps;
	int sw_ctrl_steps = 0;
	int hw_ctrl_steps = 0;
	int step_vbat = 0;
	int step_ibus = 0;
	int step_ibat = 0;
	int step_bat_reg = 0;
	int ibus_total = 0;
	int effective_fcc_val = 0;
	int effective_fcc_taper = 0;
	int thermal_level = 0;
	static int curr_fcc_limit, curr_ibus_limit, ibus_limit;

	effective_fcc_val = usbpd_get_effective_fcc_val(pdpm);

	if (effective_fcc_val > 0) {
		curr_fcc_limit = min(pm_config.bat_curr_lp_lmt, effective_fcc_val);
		if (pdpm->cp.sc8551_charge_mode == SC8551_CHARGE_MODE_BYPASS) {
			curr_ibus_limit = curr_fcc_limit;
		} else {
			curr_ibus_limit = curr_fcc_limit >> 1;
			curr_ibus_limit += pm_config.bus_curr_compensate;
		}
	}
	curr_ibus_limit = min(curr_ibus_limit, pdpm->apdo_max_curr);
	ibus_limit = curr_ibus_limit;

	if (pdpm->cp.vbat_volt > pm_config.bat_volt_lp_lmt - BQ_TAPER_HYS_MV) {
		if (ibus_lmt_change_timer++ > IBUS_CHANGE_TIMEOUT && !pdpm->disable_taper_fcc) {
			ibus_lmt_change_timer = 0;
			ibus_limit = curr_ibus_limit - 100;
			effective_fcc_taper = usbpd_get_effective_fcc_val(pdpm);
			effective_fcc_taper -= BQ_TAPER_DECREASE_STEP_MA;
			pr_err("bq set taper fcc to: %d mA\n", effective_fcc_taper);
			if (pdpm->fcc_votable)
				vote(pdpm->fcc_votable, BQ_TAPER_FCC_VOTER,
					true, effective_fcc_taper * 1000);
		}
	} else if (pdpm->cp.vbat_volt < pm_config.bat_volt_lp_lmt - 250) {
		ibus_limit = curr_ibus_limit + 100;
		ibus_lmt_change_timer = 0;
	} else {
		ibus_lmt_change_timer = 0;
	}

	ibus_limit = min(ibus_limit, pdpm->apdo_max_curr);

	pr_err("chg_mode:%d, curr_ibus_limit:%d, ibus_limit:%d, bat_curr_lp_lmt:%d, effective_fcc_val:%d, apdo_max_curr:%d\n",
			pdpm->cp.sc8551_charge_mode, curr_ibus_limit, ibus_limit,
			pm_config.bat_curr_lp_lmt, effective_fcc_val, pdpm->apdo_max_curr);

	if (pdpm->cp.vbat_volt > pm_config.bat_volt_lp_lmt)
		step_vbat = -pm_config.fc2_steps;
	else if (pdpm->cp.vbat_volt < pm_config.bat_volt_lp_lmt - 10)
		step_vbat = pm_config.fc2_steps;

	if (pdpm->cp.ibat_curr < curr_fcc_limit)
		step_ibat = pm_config.fc2_steps;
	else if (pdpm->cp.ibat_curr > curr_fcc_limit + 50)
		step_ibat = -pm_config.fc2_steps;

	ibus_total = pdpm->cp.ibus_curr;
	if (pm_config.cp_sec_enable)
		ibus_total += pdpm->cp_sec.ibus_curr;

	if (ibus_total < ibus_limit - 50)
		step_ibus = pm_config.fc2_steps;
	else if (ibus_total > ibus_limit)
		step_ibus = -pm_config.fc2_steps;

	if (pdpm->cp.vbat_reg)
		step_bat_reg = 3 * (-pm_config.fc2_steps);
	else
		step_bat_reg = pm_config.fc2_steps;

	sw_ctrl_steps = min(min(step_vbat, step_ibus), step_ibat);
	sw_ctrl_steps = min(sw_ctrl_steps, step_bat_reg);

	pr_err("vbus:%d, ibus:%d(m:%d,s:%d), vbat:%d(reg:%d), ibat:%d\n",
			pdpm->cp.vbus_volt, ibus_total, pdpm->cp.ibus_curr, pdpm->cp_sec.ibus_curr,
			pdpm->cp.vbat_volt, pdpm->cp.vbat_reg, pdpm->cp.ibat_curr);
	pr_err("sw_ctrl_steps:%d, step_vbat:%d, step_ibus:%d, step_ibat:%d, step_bat_reg:%d\n",
			sw_ctrl_steps, step_vbat, step_ibus, step_ibat, step_bat_reg);

	/* hardware alarm loop */

	if (pdpm->cp.bus_ocp_alarm || pdpm->cp.bus_ovp_alarm)
		hw_ctrl_steps = -pm_config.fc2_steps;
	else
		hw_ctrl_steps = pm_config.fc2_steps;
	pr_err("hw_ctrl_steps:%d\n", hw_ctrl_steps);

	usbpd_pm_check_cp_enabled(pdpm);

	if (pm_config.cp_sec_enable)
		usbpd_pm_check_cp_sec_enabled(pdpm);

	pd_get_batt_current_thermal_level(pdpm, &thermal_level);
	pdpm->is_temp_out_fc2_range = pd_disable_cp_by_jeita_status(pdpm);
	pr_err("is_temp_out_fc2_range = %d, thermal_level = %d\n",
			pdpm->is_temp_out_fc2_range, thermal_level);

	usbpd_pm_check_slowly_charging_enabled(pdpm);

	if (pdpm->cp.bat_therm_fault) { /* battery overheat, stop charge*/
		pr_err("bat_therm_fault:%d\n", pdpm->cp.bat_therm_fault);
		return PM_ALGO_RET_THERM_FAULT;
	} else if (pdpm->is_temp_out_fc2_range
			|| (thermal_level >= MAX_THERMAL_LEVEL
			&& pdpm->cp.sc8551_charge_mode != SC8551_CHARGE_MODE_BYPASS)
			|| (thermal_level >= BYPASS_THERMAL_EXIT_LEVEL
			&& pdpm->cp.sc8551_charge_mode == SC8551_CHARGE_MODE_BYPASS)) {
		pr_err("thermal level too high or batt temp is out of fc2 range\n");
		return PM_ALGO_RET_CHG_DISABLED;
	} else if (pdpm->cp.bat_ocp_fault || pdpm->cp.bus_ocp_fault
			|| pdpm->cp.bat_ovp_fault || pdpm->cp.bus_ovp_fault) {
		pr_err("bat_ocp_fault:%d, bus_ocp_fault:%d, bat_ovp_fault:%d, bus_ovp_fault:%d\n",
				pdpm->cp.bat_ocp_fault, pdpm->cp.bus_ocp_fault,
				pdpm->cp.bat_ovp_fault, pdpm->cp.bus_ovp_fault);
		return PM_ALGO_RET_OTHER_FAULT; /* go to switch, and try to ramp up*/
	} else if (!pdpm->cp.charge_enabled || (pm_config.cp_sec_enable && !pdpm->cp_sec.charge_enabled && !pdpm->cp_sec_stopped)) {
		pr_err("cp.charge_enabled:%d, cp_sec.charge_enabled:%d\n",
				pdpm->cp.charge_enabled, pdpm->cp_sec.charge_enabled);
		return PM_ALGO_RET_CHG_DISABLED;
	} else if (pdpm->sw.slowly_charging) {
		pr_err("slowly charging enabled[%d]\n", pdpm->sw.slowly_charging);
		return PM_ALGO_RET_SLOWLY_CHARGING;
	}

	if (pdpm->cp.vbat_volt > pm_config.bat_volt_lp_lmt
			&& is_cool_charge(pdpm)) {
		if (cool_overcharge_timer++ > TAPER_TIMEOUT) {
			pr_err("cool overcharge\n");
			cool_overcharge_timer = 0;
			return PM_ALGO_RET_TAPER_DONE;
			}
	} else {
		cool_overcharge_timer = 0;
	}

	if (pdpm->cp.vbat_volt > pm_config.bat_volt_lp_lmt - TAPER_VOL_HYS
			&& pdpm->cp.ibat_curr < pm_config.fc2_taper_current) {
		if (fc2_taper_timer++ > TAPER_TIMEOUT) {
			pr_err("charge pump taper charging done\n");
			fc2_taper_timer = 0;
			return PM_ALGO_RET_TAPER_DONE;
		}
	} else {
		fc2_taper_timer = 0;
	}

	ret = usbpd_pm_switch_cp_charge_mode(pdpm);
	if (ret > 0) {
		pr_err("charge mode changed, re-run statemachine\n");
		return PM_ALGO_RET_CHG_DISABLED;
	}

	steps = min(sw_ctrl_steps, hw_ctrl_steps);
	pr_err("steps: %d, sw_ctrl_steps:%d, hw_ctrl_steps:%d\n", steps, sw_ctrl_steps, hw_ctrl_steps);
	pdpm->request_voltage += steps * STEP_MV;

	// if (pdpm->apdo_max_volt == PPS_VOL_MAX)
	// 	pdpm->apdo_max_volt = pdpm->apdo_max_volt - PPS_VOL_HYS;

	if (pdpm->request_voltage > pdpm->apdo_max_volt)
		pdpm->request_voltage = pdpm->apdo_max_volt;

	pdpm->request_current = min(pdpm->apdo_max_curr, curr_ibus_limit);

	pr_err("steps:%d, pdpm->request_voltage:%d, pdpm->request_current:%d\n",
			steps, pdpm->request_voltage, pdpm->request_current);

	return PM_ALGO_RET_OK;
}

static const unsigned char *pm_str[] = {
	"PD_PM_STATE_ENTRY",
	"PD_PM_STATE_FC2_ENTRY",
	"PD_PM_STATE_FC2_ENTRY_1",
	"PD_PM_STATE_FC2_ENTRY_2",
	"PD_PM_STATE_FC2_ENTRY_3",
	"PD_PM_STATE_FC2_TUNE",
	"PD_PM_STATE_FC2_EXIT",
};

static void usbpd_pm_move_state(struct usbpd_pm *pdpm, enum pm_state state)
{
#if 1
	pr_info("state change:%s -> %s\n",
		pm_str[pdpm->state], pm_str[state]);
#endif
	pdpm->state = state;
}
extern bool is_std_battery;
static int usbpd_pm_sm(struct usbpd_pm *pdpm)
{
	int ret;
	int rc = 0;
	static int tune_vbus_retry;
	static bool stop_sw;
	static bool recover;
	int effective_fcc_val = 0;
	int thermal_level = 0;
	static int curr_fcc_lmt, curr_ibus_lmt, retry_count;
	static int request_fail_count = 0;
	int capacity = 0;
	union power_supply_propval pval = {0, };
	pr_err(">>>>>>>>>>>state phase :%d\n", pdpm->state);
	pr_err("charge_info:vbat:%d, ibat:%d, vbus:%d(hi_lo:%d), ibus:(%d+%d), tbat:%d, vbus_pres:%d, chg_en1:%d, chg_en2:%d, bypass_en:%d, chg_mode1:%d\n",
			pdpm->cp.vbat_volt, pdpm->cp.ibat_curr,
			pdpm->cp.vbus_volt, pdpm->cp.bus_error_status, pdpm->cp.ibus_curr, pdpm->cp_sec.ibus_curr,
			pdpm->cp.bat_temp, pdpm->cp.vbus_pres,
			pdpm->cp.charge_enabled, pdpm->cp_sec.charge_enabled,
			pdpm->cp.sc8551_bypass_charge_enable, pdpm->cp.sc8551_charge_mode);
#ifdef WT_COMPILE_FACTORY_VERSION
	pd_get_batt_capacity(pdpm, &capacity);
	if ((capacity >= 79) && ((pdpm->state != PD_PM_STATE_FC2_EXIT) && (pdpm->state != PD_PM_STATE_FC2_TUNE))) {
		stop_sw = false;
		usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
		pr_err("wt_debug : %s state phase 6, capacity >= 79, slave charger exit", __func__);
	}
#endif
	usbpd_pm_src_caps_update(pdpm);
	switch (pdpm->state) {
	case PD_PM_STATE_ENTRY:
		stop_sw = false;
		recover = false;
		request_fail_count = 0;

		usbpd_pm_check_slowly_charging_enabled(pdpm);
		pd_get_batt_current_thermal_level(pdpm, &thermal_level);
		pdpm->is_temp_out_fc2_range = pd_disable_cp_by_jeita_status(pdpm);
		pr_err("is_temp_out_fc2_range:%d\n", pdpm->is_temp_out_fc2_range);

		pd_get_batt_capacity(pdpm, &capacity);
		effective_fcc_val = usbpd_get_effective_fcc_val(pdpm);

		if (effective_fcc_val > 0) {
			curr_fcc_lmt = min(pm_config.bat_curr_lp_lmt, effective_fcc_val);
			if (pdpm->cp.sc8551_charge_mode == SC8551_CHARGE_MODE_BYPASS)
				curr_ibus_lmt = curr_fcc_lmt;
			else
				curr_ibus_lmt = curr_fcc_lmt >> 1;
			pr_err("curr_ibus_lmt:%d\n", curr_ibus_lmt);
		}

		if (pdpm->cp.vbat_volt < pm_config.min_vbat_for_cp) {
			pr_err("batt_volt %d, waiting...\n", pdpm->cp.vbat_volt);
		} else if (pdpm->cp.vbat_volt > pm_config.bat_volt_lp_lmt - 50 || capacity > 95) {
			pr_err("batt_volt or capacity %d is too high for cp, charging with switch charger\n",
					pdpm->cp.vbat_volt);
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			if (pm_config.bat_volt_lp_lmt < BAT_VOLT_LOOP_LMT)
				recover = true;
		} else if (!pd_get_bms_digest_verified(pdpm)) {
			pr_err("bms digest is not verified, waiting...\n");
		} else if (pdpm->is_temp_out_fc2_range
			|| (thermal_level >= MAX_THERMAL_LEVEL
			&& pdpm->cp.sc8551_charge_mode != SC8551_CHARGE_MODE_BYPASS)
			|| (thermal_level >= BYPASS_THERMAL_EXIT_LEVEL
			&& pdpm->cp.sc8551_charge_mode == SC8551_CHARGE_MODE_BYPASS)) {
			pr_err("thermal too high or batt temp is out of fc2 range, waiting...\n");
		} else if (pdpm->sw.slowly_charging) {
			pr_err("slowly charging feature is on, waiting...\n");
		} else if (effective_fcc_val < LOW_POWER_PPS_CURR_THR) {
			pr_err("ibat too low, waiting...\n");
		} else {
			pr_err("batt_volt-%d is ok, start flash charging\n",
					pdpm->cp.vbat_volt);
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY);
		}
		break;

	case PD_PM_STATE_FC2_ENTRY:
		if (pm_config.fc2_disable_sw) {
			if (!pdpm->sw.charge_limited) {
				usbpd_pm_limit_sw(pdpm, true);
				usbpd_pm_update_sw_status(pdpm);
			}
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY_1);
		} else {
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY_1);
		}
		retry_count = 0;
		break;

	case PD_PM_STATE_FC2_ENTRY_1:
		usbpd_pm_switch_cp_charge_mode(pdpm);
		if (pdpm->cp.sc8551_bypass_charge_enable == 1
				&& pdpm->cp.sc8551_charge_mode == SC8551_CHARGE_MODE_BYPASS) {
			if (curr_fcc_lmt > XIAOMI_LOW_POWER_PPS_CURR_MAX)
				curr_ibus_lmt = XIAOMI_LOW_POWER_PPS_CURR_MAX;
			else
				curr_ibus_lmt = curr_fcc_lmt;
			pdpm->request_voltage = pdpm->cp.vbat_volt + BUS_VOLT_INIT_UP * 2;
			pdpm->request_current = min(pdpm->apdo_max_curr, curr_ibus_lmt);
			pdpm->request_current = min(pdpm->request_current, MAX_BYPASS_CURRENT_MA);
		} else {
			curr_ibus_lmt = curr_fcc_lmt >> 1;
			pdpm->request_voltage = pdpm->cp.vbat_volt * 2 + BUS_VOLT_INIT_UP;
			pdpm->request_current = min(pdpm->apdo_max_curr, curr_ibus_lmt);
		}

		usbpd_select_pdo(pdpm->pd, pdpm->apdo_selected_pdo,
				pdpm->request_voltage * 1000, pdpm->request_current * 1000);
		pr_err("request_voltage:%d, request_current:%d\n",
				pdpm->request_voltage, pdpm->request_current);

		usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY_2);

		tune_vbus_retry = 0;
		break;

	case PD_PM_STATE_FC2_ENTRY_2:
#ifdef CONFIG_K6_CHARGE
		pr_info("bus_err_st:%d, req_vol:%dmV, cur_vol:%d, req_curr:%d, vbat:%d, retry:%d\n",
				pdpm->cp.bus_error_status, pdpm->request_voltage, pdpm->cp.vbus_volt, pdpm->request_current, pdpm->cp.vbat_volt, tune_vbus_retry);
		if (pdpm->cp.bus_error_status == VBUS_ERROR_LOW ||
				(pdpm->cp.vbus_volt < pdpm->cp.vbat_volt * 2 + 300 && pdpm->cp.sc8551_charge_mode != SC8551_CHARGE_MODE_BYPASS) ||
				(pdpm->cp.vbus_volt < pdpm->cp.vbat_volt + BUS_VOLT_INIT_UP && pdpm->cp.sc8551_charge_mode == SC8551_CHARGE_MODE_BYPASS)) {
			tune_vbus_retry++;
			pdpm->request_voltage += STEP_MV;
			usbpd_select_pdo(pdpm->pd, pdpm->apdo_selected_pdo,
						pdpm->request_voltage * 1000,
						pdpm->request_current * 1000);
		} else if (pdpm->cp.bus_error_status == VBUS_ERROR_HIGH) {
			tune_vbus_retry++;
			pdpm->request_voltage -= STEP_MV;
			usbpd_select_pdo(pdpm->pd, pdpm->apdo_selected_pdo,
						pdpm->request_voltage * 1000,
						pdpm->request_current * 1000);
		} else {
			pr_err("adapter volt tune ok, retry %d times\n", tune_vbus_retry);
					usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY_3);
			break;
		}
#else
		pr_err("tune adapter volt %d , vbatt %d\n",
					pdpm->cp.vbus_volt, pdpm->cp.vbat_volt);
		if (pdpm->cp.vbus_volt < (pdpm->cp.vbat_volt * 2 + BUS_VOLT_INIT_UP - 50)) {
			tune_vbus_retry++;
			pdpm->request_voltage += STEP_MV;
			usbpd_select_pdo(pdpm->pd, pdpm->apdo_selected_pdo,
						pdpm->request_voltage * 1000,
						pdpm->request_current * 1000);
		} else if (pdpm->cp.vbus_volt > (pdpm->cp.vbat_volt * 2 + BUS_VOLT_INIT_UP + 200)) {
			tune_vbus_retry++;
			pdpm->request_voltage -= STEP_MV;
			usbpd_select_pdo(pdpm->pd, pdpm->apdo_selected_pdo,
						pdpm->request_voltage * 1000,
						pdpm->request_current * 1000);
		} else {
			pr_err("adapter volt tune ok, retry %d times\n", tune_vbus_retry);
					usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY_3);
			break;
		}
#endif
		if (tune_vbus_retry > 60) {
			if (retry_count < 1) {
				usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY_1);
				retry_count++;
				pr_err("Failed to tune adapter volt into valid range, retry again\n");
			} else {
				pr_err("Failed to tune adapter volt into valid range, charge with switching charger\n");
				usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			}
		}
		break;

	case PD_PM_STATE_FC2_ENTRY_3:
		if (is_std_battery) {
				pr_err("gx sec cp reinit...\n");
				pval.intval = 1;
				power_supply_set_property(pdpm->cp_sec_psy,POWER_SUPPLY_PROP_SEC_CP_INIT_DEVICE, &pval);
		} else {
				pr_err("gx exit cp \n");
				usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
		}
		if (pm_config.cp_sec_enable && !pdpm->cp_sec.charge_enabled) {
			usbpd_pm_enable_cp_sec(pdpm, true);
			msleep(30);
			usbpd_pm_check_cp_sec_enabled(pdpm);
		}

		if (!pdpm->cp.charge_enabled) {
			usbpd_pm_enable_cp(pdpm, true);
			msleep(30);
			usbpd_pm_check_cp_enabled(pdpm);
		}

		if (pdpm->cp.charge_enabled) {
			if (pm_config.fc2_disable_sw) {
				if (pdpm->sw.charge_enabled) {
					usbpd_pm_enable_sw(pdpm, false);
					usbpd_pm_update_sw_status(pdpm);
				}
			}
			if ((pm_config.cp_sec_enable && pdpm->cp_sec.charge_enabled)
					|| !pm_config.cp_sec_enable) {
				usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_TUNE);
				ibus_lmt_change_timer = 0;
				fc2_taper_timer = 0;
			}
		}
		break;

	case PD_PM_STATE_FC2_TUNE:
#if 0
		if (pdpm->cp.vbat_volt < pm_config.min_vbat_for_cp - 400) {
			usbpd_pm_move_state(PD_PM_STATE_SW_ENTRY);
			break;
		}
#endif
		if (!is_std_battery) {
				pr_err("gx exit cp \n");
				usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
		}

		usbpd_update_pps_status(pdpm);

		ret = usbpd_pm_fc2_charge_algo(pdpm);
		if (ret == PM_ALGO_RET_THERM_FAULT) {
			pr_err("Move to stop charging:%d\n", ret);
			stop_sw = true;
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			break;
		} else if (ret == PM_ALGO_RET_OTHER_FAULT || ret == PM_ALGO_RET_TAPER_DONE) {
			pr_err("Move to switch charging:%d\n", ret);
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			break;
		} else if (ret == PM_ALGO_RET_CHG_DISABLED) {
			pr_err("Move to switch charging, will try to recover flash charging:%d\n",
					ret);
			recover = true;
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			break;
		} else if (ret == PM_ALGO_RET_SLOWLY_CHARGING) {
			recover = true;
			pr_err("Slow Charging Feature is running %d\n", ret);
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
		} else {
			ret = usbpd_select_pdo(pdpm->pd, pdpm->apdo_selected_pdo, pdpm->request_voltage * 1000, pdpm->request_current * 1000);
			if (ret && ret != -ESERVERFAULT && pdpm->cp.vbus_volt <= VALID_VBUS_THRESHOLD) {
				pr_err("failed request_voltage:%d, request_current:%d\n", pdpm->request_voltage, pdpm->request_current);
				request_fail_count++;
				if (request_fail_count > 1) {
					pr_err("failed to request pdo\n");
					request_fail_count = 0;
					usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
					break;
				}
			} else {
				request_fail_count = 0;
				pr_err("sucess request_voltage:%d, request_current:%d\n", pdpm->request_voltage, pdpm->request_current);
			}
		}
#ifdef WT_COMPILE_FACTORY_VERSION
		if (capacity >= 79) {
			stop_sw = false;
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			pr_err("wt_debug : %s capacity >= 79, slave charger exit", __func__);
		}
#endif
		/*stop second charge pump if either of ibus is lower than 400ma during CV*/
		if (pm_config.cp_sec_enable && pdpm->cp_sec.charge_enabled
				&& pdpm->cp.vbat_volt > pm_config.bat_volt_lp_lmt - TAPER_WITH_IBUS_HYS
				&& (pdpm->cp.ibus_curr < TAPER_IBUS_THR
					|| pdpm->cp_sec.ibus_curr < TAPER_IBUS_THR)) {
			pr_err("second cp is disabled due to ibus < 450mA\n");
			usbpd_pm_enable_cp_sec(pdpm, false);
			usbpd_pm_check_cp_sec_enabled(pdpm);
			pdpm->cp_sec_stopped = true;
		}
		break;

	case PD_PM_STATE_FC2_EXIT:
		usbpd_select_pdo(pdpm->pd, 1, 0, 0);
		if (pdpm->fcc_votable)
			vote(pdpm->fcc_votable, BQ_TAPER_FCC_VOTER,
					false, 0);		

#ifdef WT_COMPILE_FACTORY_VERSION
		if (!stop_sw && capacity >= 79) {
				usbpd_pm_enable_sw(pdpm, true);
				pr_err("wt_debug : %s capacity >= 79, slave charger exit, enable sw.", __func__);
		} else {
			if (!stop_sw && (!pdpm->sw.charge_enabled || pdpm->sw.charge_limited)) {
				usbpd_pm_enable_sw(pdpm, true);
			}
			if (stop_sw && (pdpm->sw.charge_enabled || pdpm->sw.charge_limited))
				usbpd_pm_enable_sw(pdpm, false);	
		}
#else
		if (!stop_sw && (!pdpm->sw.charge_enabled || pdpm->sw.charge_limited)) {
			usbpd_pm_enable_sw(pdpm, true);
		}
		if (stop_sw && (pdpm->sw.charge_enabled || pdpm->sw.charge_limited))
			usbpd_pm_enable_sw(pdpm, false);	
#endif
		usbpd_pm_update_sw_status(pdpm);

		if (pdpm->cp.charge_enabled) {
			usbpd_pm_enable_cp(pdpm, false);
			usbpd_pm_check_cp_enabled(pdpm);
		}

		if (pm_config.cp_sec_enable && pdpm->cp_sec.charge_enabled) {
			usbpd_pm_enable_cp_sec(pdpm, false);
			usbpd_pm_check_cp_sec_enabled(pdpm);
		}

		if (recover)
			usbpd_pm_move_state(pdpm, PD_PM_STATE_ENTRY);
		else
			rc = 1;

		break;
	default:
		usbpd_pm_move_state(pdpm, PD_PM_STATE_ENTRY);
		break;
	}

	return rc;
}

static void usbpd_pm_workfunc(struct work_struct *work)
{
	struct usbpd_pm *pdpm = container_of(work, struct usbpd_pm,
					pm_work.work);

	usbpd_pm_update_sw_status(pdpm);
	usbpd_pm_update_cp_status(pdpm);
	usbpd_pm_update_cp_sec_status(pdpm);


	pr_info("%s:pd_bat_volt_lp_lmt=%d, vbatt_now=%d\n",
			__func__, pm_config.bat_volt_lp_lmt, pdpm->cp.vbat_volt);

	if (!usbpd_pm_sm(pdpm) && pdpm->pd_active)
		schedule_delayed_work(&pdpm->pm_work,
				msecs_to_jiffies(PM_WORK_RUN_INTERVAL));
}

static void usbpd_pm_disconnect(struct usbpd_pm *pdpm)
{
	union power_supply_propval pval = {0, };

	cancel_delayed_work_sync(&pdpm->pm_work);

	if (pdpm->fcc_votable) {
		vote(pdpm->fcc_votable, BQ_TAPER_FCC_VOTER,
				false, 0);
		vote(pdpm->fcc_votable, PD_UNVERIFED_VOTER,
				false, 0);
	}
	pdpm->pps_supported = false;
	pdpm->jeita_triggered = false;
	pdpm->is_temp_out_fc2_range = false;
	pdpm->apdo_selected_pdo = 0;
	memset(&pdpm->pdo, 0, sizeof(pdpm->pdo));
	pm_config.bat_curr_lp_lmt = pdpm->bat_curr_max;
	if (!pdpm->sw.charge_enabled || pdpm->sw.charge_limited) {
		usbpd_pm_enable_sw(pdpm, true);
		usbpd_pm_check_sw_enabled(pdpm);
	}

	pval.intval = 0;
	power_supply_set_property(pdpm->usb_psy,
			POWER_SUPPLY_PROP_APDO_MAX, &pval);

	usbpd_pm_enable_cp(pdpm, false);

	usbpd_pm_move_state(pdpm, PD_PM_STATE_ENTRY);
}

static void usbpd_pd_contact(struct usbpd_pm *pdpm, bool connected)
{
	pdpm->pd_active = connected;

	pr_info("pd_active = %d\n", connected);

	if (connected) {
		usbpd_pm_evaluate_src_caps(pdpm);
		if (pdpm->pps_supported)
			schedule_delayed_work(&pdpm->pm_work, 0);
	} else {
		usbpd_pm_disconnect(pdpm);
	}
}

static void usbpd_pps_non_verified_contact(struct usbpd_pm *pdpm, bool connected)
{
	pdpm->pd_active = connected;

	pr_info("pd_active = %d\n", connected);

	if (connected) {
		usbpd_pm_evaluate_src_caps(pdpm);
		
		if (pdpm->pps_supported){
			if (!pdpm->fcc_votable)
				pdpm->fcc_votable = find_votable("FCC");

			if ((pdpm->apdo_max_volt / 1000) * (pdpm->apdo_max_curr / 1000) < 33) {
				if (pdpm->fcc_votable)
					vote(pdpm->fcc_votable, PD_UNVERIFED_VOTER, true, PD_UNVERIFED_CURRENT_LOW);
			} else {
				if (pdpm->fcc_votable)
					vote(pdpm->fcc_votable, PD_UNVERIFED_VOTER, true, PD_UNVERIFED_CURRENT_HIGH);
			}
			schedule_delayed_work(&pdpm->pm_work, 5*HZ);
		}
	} else {
		usbpd_pm_disconnect(pdpm);
	}
}

static void cp_psy_change_work(struct work_struct *work)
{
	struct usbpd_pm *pdpm = container_of(work, struct usbpd_pm,
					cp_psy_change_work);
	pr_err("%s\n", __func__);
	pdpm->psy_change_running = false;
}

static void usb_psy_change_work(struct work_struct *work)
{
	struct usbpd_pm *pdpm = container_of(work, struct usbpd_pm,
					usb_psy_change_work);
	union power_supply_propval val = {0,};
	union power_supply_propval pd_auth_val = {0,};
	union power_supply_propval hardreset_val = {0,};
	int ret = 0;

	pr_err("%s\n", __func__);
	ret = power_supply_get_property(pdpm->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);
	if (ret) {
		pr_err("Failed to read typec power role\n");
		goto out;
	}

	if (val.intval != POWER_SUPPLY_TYPEC_PR_SINK &&
			val.intval != POWER_SUPPLY_TYPEC_PR_DUAL)
		goto out;

	ret = power_supply_get_property(pdpm->usb_psy,
			POWER_SUPPLY_PROP_PD_ACTIVE, &val);
	if (ret) {
		pr_err("Failed to get usb pd active state\n");
		goto out;
	}

	ret = power_supply_get_property(pdpm->usb_psy,
			POWER_SUPPLY_PROP_PD_AUTHENTICATION, &pd_auth_val);
	if (ret) {
		pr_err("Failed to read typec power role\n");
		goto out;
	}

	ret = power_supply_get_property(pdpm->usb_psy,
			POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &hardreset_val);
	if (ret) {
		pr_err("Failed to read hardreset\n");
		goto out;
	}

	pr_err("%s pd_active:%d, pd_auth:%d, pps:%d, hardreset:%d\n", __func__,
				pdpm->pd_active, pd_auth_val.intval, val.intval, hardreset_val.intval);
	if (!pdpm->pd_active && (pd_auth_val.intval == 1)
			&& (val.intval == POWER_SUPPLY_PD_PPS_ACTIVE)
			&& !hardreset_val.intval)
		usbpd_pd_contact(pdpm, true);
	else if (!pdpm->pd_active
			&& (val.intval == POWER_SUPPLY_PD_PPS_ACTIVE)
			&& !hardreset_val.intval)
		usbpd_pps_non_verified_contact(pdpm, true);
	else if (pdpm->pd_active && (!val.intval || hardreset_val.intval))
		usbpd_pd_contact(pdpm, false);


out:
	pdpm->psy_change_running = false;
}

static int usbpd_psy_notifier_cb(struct notifier_block *nb,
			unsigned long event, void *data)
{
	struct usbpd_pm *pdpm = container_of(nb, struct usbpd_pm, nb);
	struct power_supply *psy = data;
	unsigned long flags;

	if (event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;

	usbpd_check_cp_psy(pdpm);
	usbpd_check_usb_psy(pdpm);

	if (!pdpm->cp_psy || !pdpm->usb_psy)
		return NOTIFY_OK;

	if (psy == pdpm->cp_psy || psy == pdpm->usb_psy) {
		spin_lock_irqsave(&pdpm->psy_change_lock, flags);
		if (!pdpm->psy_change_running) {
			pdpm->psy_change_running = true;
			if (psy == pdpm->cp_psy)
				schedule_work(&pdpm->cp_psy_change_work);
			else
				schedule_work(&pdpm->usb_psy_change_work);
		}
		spin_unlock_irqrestore(&pdpm->psy_change_lock, flags);
	}

	return NOTIFY_OK;
}

static int pd_policy_parse_dt(struct usbpd_pm *pdpm)
{
	struct device_node *node = pdpm->dev->of_node;
	int rc = 0;

	if (!node) {
		pr_err("device tree node missing\n");
		return -EINVAL;
	}

	rc = of_property_read_u32(node,
			"mi,pd-bat-volt-max", &pdpm->bat_volt_max);
	if (rc < 0)
		pr_err("pd-bat-volt-max property missing, use default val\n");
	else
		pm_config.bat_volt_lp_lmt = pdpm->bat_volt_max;
	pr_info("pm_config.bat_volt_lp_lmt:%d\n", pm_config.bat_volt_lp_lmt);

	rc = of_property_read_u32(node,
			"mi,pd-bat-curr-max", &pdpm->bat_curr_max);
	if (rc < 0)
		pr_err("pd-bat-curr-max property missing, use default val\n");
	else
		pm_config.bat_curr_lp_lmt = pdpm->bat_curr_max;
	pr_info("pm_config.bat_curr_lp_lmt:%d\n", pm_config.bat_curr_lp_lmt);

	rc = of_property_read_u32(node,
			"mi,pd-bus-volt-max", &pdpm->bus_volt_max);
	if (rc < 0)
		pr_err("pd-bus-volt-max property missing, use default val\n");
	else
		pm_config.bus_volt_lp_lmt = pdpm->bus_volt_max;
	pr_info("pm_config.bus_volt_lp_lmt:%d\n", pm_config.bus_volt_lp_lmt);

	rc = of_property_read_u32(node,
			"mi,pd-bus-curr-max", &pdpm->bus_curr_max);
	if (rc < 0)
		pr_err("pd-bus-curr-max property missing, use default val\n");
	else
		pm_config.bus_curr_lp_lmt = pdpm->bus_curr_max;
	pr_info("pm_config.bus_curr_lp_lmt:%d\n", pm_config.bus_curr_lp_lmt);

	rc = of_property_read_u32(node,
			"mi,pd-bus-curr-compensate", &pdpm->bus_curr_compensate);
	if (rc < 0)
		pr_err("pd-bus-curr-compensate property missing, use default val\n");
	else
		pm_config.bus_curr_compensate = pdpm->bus_curr_compensate;
	pr_info("pm_config.bus_curr_compensate:%d\n", pm_config.bus_curr_compensate);

	pdpm->cp_sec_enable = of_property_read_bool(node,
				"mi,cp-sec-enable");
	pm_config.cp_sec_enable = pdpm->cp_sec_enable;

	rc = of_property_read_u32(node,
			"mi,pd-ffc-bat-volt-max", &pdpm->ffc_bat_volt_max);
	pr_info("pdpm->ffc_bat_volt_max:%d\n",
				pdpm->ffc_bat_volt_max);

	pdpm->disable_taper_fcc = of_property_read_bool(node, "mi,disable-taper-fcc");
	pr_info("disable_pd_sm_taper_fcc:%d\n", pdpm->disable_taper_fcc);

	return rc;
}

static int usbpd_pm_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct usbpd_pm *pdpm;

	pr_info("%s enter\n", __func__);

	pdpm = kzalloc(sizeof(struct usbpd_pm), GFP_KERNEL);
	if (!pdpm)
		return -ENOMEM;

	__pdpm = pdpm;

	pdpm->dev = dev;

	ret = pd_policy_parse_dt(pdpm);
	if (ret < 0) {
		pr_err("Couldn't parse device tree rc=%d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, pdpm);

	spin_lock_init(&pdpm->psy_change_lock);

	usbpd_check_cp_psy(pdpm);
	usbpd_check_cp_sec_psy(pdpm);
	usbpd_check_usb_psy(pdpm);

	INIT_WORK(&pdpm->cp_psy_change_work, cp_psy_change_work);
	INIT_WORK(&pdpm->usb_psy_change_work, usb_psy_change_work);
	INIT_DELAYED_WORK(&pdpm->pm_work, usbpd_pm_workfunc);

	pdpm->nb.notifier_call = usbpd_psy_notifier_cb;
	power_supply_reg_notifier(&pdpm->nb);

	return ret;
}

static int usbpd_pm_remove(struct platform_device *pdev)
{
	power_supply_unreg_notifier(&__pdpm->nb);
	cancel_delayed_work(&__pdpm->pm_work);
	cancel_work(&__pdpm->cp_psy_change_work);
	cancel_work(&__pdpm->usb_psy_change_work);

	return 0;
}

static const struct of_device_id usbpd_pm_of_match[] = {
	{ .compatible = "xiaomi,usbpd-pm", },
	{},
};

static struct platform_driver usbpd_pm_driver = {
	.driver = {
		.name = "usbpd-pm",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(usbpd_pm_of_match),
	},
	.probe = usbpd_pm_probe,
	.remove = usbpd_pm_remove,
};

static int __init usbpd_pm_init(void)
{
	return platform_driver_register(&usbpd_pm_driver);
}

late_initcall(usbpd_pm_init);

static void __exit usbpd_pm_exit(void)
{
	return platform_driver_unregister(&usbpd_pm_driver);
}
module_exit(usbpd_pm_exit);

MODULE_AUTHOR("Fei Jiang<jiangfei1@xiaomi.com>");
MODULE_DESCRIPTION("Xiaomi usb pd statemachine for bq");
MODULE_LICENSE("GPL");

