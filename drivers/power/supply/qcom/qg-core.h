/* Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (C) 2021 XiaoMi, Inc.
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
#ifndef __QG_CORE_H__
#define __QG_CORE_H__

#include <linux/kernel.h>
#include "fg-alg.h"
#include "qg-defs.h"

#define NTC_COMP_HIGH_TEMP		500
#define NTC_COMP_LOW_TEMP		200
#define TEMP_COMP_TIME			5

enum ffc_current_cfg {
	TEMP_THRESHOLD,
	LOW_TEMP_FULL_CURRENT,
	HIGH_TEMP_FULL_CURRENT,
	LOW_TEMP_TERMINAL_CURRENT,
	HIGH_TEMP_TERMINAL_CURRENT,
	USE_DTS_CONFIG,
};

struct qg_batt_props {
	const char		*batt_type_str;
	int			float_volt_uv;
	int			vbatt_full_mv;
	int			fastchg_curr_ma;
	int			qg_profile_version;
	int			nom_cap_uah;
	int			ffc_current_cfg[USE_DTS_CONFIG + 1];
};

struct qg_irq_info {
	const char		*name;
	const irq_handler_t	handler;
	const bool		wake;
	int			irq;
};

struct qg_dt {
	int			vbatt_empty_mv;
	int			vbatt_empty_cold_mv;
	int			vbatt_low_mv;
	int			vbatt_low_cold_mv;
	int			vbatt_cutoff_mv;
	int			iterm_ma;
	int			s2_fifo_length;
	int			s2_vbat_low_fifo_length;
	int			s2_acc_length;
	int			s2_acc_intvl_ms;
	int			sleep_s2_fifo_length;
	int			sleep_s2_acc_length;
	int			sleep_s2_acc_intvl_ms;
	int			fast_chg_s2_fifo_length;
	int			ocv_timer_expiry_min;
	int			ocv_tol_threshold_uv;
	int			s3_entry_fifo_length;
	int			s3_entry_ibat_ua;
	int			s3_exit_ibat_ua;
	int			delta_soc;
	int			rbat_conn_mohm;
	int			ignore_shutdown_soc_secs;
	int			shutdown_temp_diff;
	int			cold_temp_threshold;
	int			esr_qual_i_ua;
	int			esr_qual_v_uv;
	int			esr_disable_soc;
	int			esr_min_ibat_ua;
	int			shutdown_soc_threshold;
	int			min_sleep_time_secs;
	int			sys_min_volt_mv;
	int			fvss_vbat_mv;
	int			tcss_entry_soc;
	bool			hold_soc_while_full;
	bool			linearize_soc;
	bool			cl_disable;
	bool			cl_feedback_on;
	bool			esr_disable;
	bool			esr_discharge_enable;
	bool			qg_ext_sense;
	bool			use_s7_ocv;
	bool			qg_sleep_config;
	bool			qg_fast_chg_cfg;
	bool			fvss_enable;
	bool			multi_profile_load;
	bool			tcss_enable;
	bool			bass_enable;
	bool			disable_hold_full;
	bool                    temp_battery_id;
	bool			qg_page0_unused;
	bool			ffc_iterm_change_by_temp;
	bool			software_optimize_ffc_qg_iterm;
	bool			shutdown_delay_enable;
	int			*dec_rate_seq;
	int			dec_rate_len;
};

struct qg_esr_data {
	u32			pre_esr_v;
	u32			pre_esr_i;
	u32			post_esr_v;
	u32			post_esr_i;
	u32			esr;
	bool			valid;
};

#define BATT_MA_AVG_SAMPLES	8
struct batt_params {
	bool			update_now;
	int			batt_raw_soc;
	int			batt_soc;
	int			samples_num;
	int			samples_index;
	int			batt_ma_avg_samples[BATT_MA_AVG_SAMPLES];
	int			batt_ma_avg;
	int			batt_ma_prev;
	int			batt_ma;
	int			batt_mv;
	int			batt_temp;
	struct timespec		last_soc_change_time;
};

struct qpnp_qg {
	struct device		*dev;
	struct pmic_revid_data	*pmic_rev_id;
	struct regmap		*regmap;
	struct qpnp_vadc_chip	*vadc_dev;
	struct soh_profile	*sp;
	struct power_supply	*qg_psy;
	struct class		*qg_class;
	struct device		*qg_device;
	struct cdev		qg_cdev;
	struct device_node	*batt_node;
	dev_t			dev_no;
	struct batt_params	param;
	struct delayed_work	soc_monitor_work;
	struct delayed_work	force_shutdown_work;
	struct work_struct	udata_work;
	struct work_struct	scale_soc_work;
	struct work_struct	qg_status_change_work;
	struct delayed_work	qg_sleep_exit_work;
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	struct delayed_work	battery_authentic_work;
	int			battery_authentic_result;
	struct delayed_work	ds_romid_work;
	unsigned char		ds_romid[8];
	struct delayed_work	ds_status_work;
	unsigned char		ds_status[8];
	struct delayed_work	ds_page0_work;
	unsigned char		ds_page0[16];
	struct delayed_work	profile_load_work;
	bool				profile_judge_done;
#endif
	struct notifier_block	nb;
	struct mutex		bus_lock;
	struct mutex		data_lock;
	struct mutex		soc_lock;
	wait_queue_head_t	qg_wait_q;
	struct votable		*awake_votable;
	struct votable		*vbatt_irq_disable_votable;
	struct votable		*fifo_irq_disable_votable;
	struct votable		*good_ocv_irq_disable_votable;
	u32			qg_base;
	u8			qg_subtype;

	/* local data variables */
	u32			batt_id_ohm;
	struct qg_kernel_data	kdata;
	struct qg_user_data	udata;
	struct power_supply	*batt_psy;
	struct power_supply	*usb_psy;
	struct power_supply	*dc_psy;
	struct power_supply	*parallel_psy;
#ifdef CONFIG_BATT_VERIFY_BY_DS28E16
	struct power_supply *max_verify_psy;
#endif
	struct qg_esr_data	esr_data[QG_MAX_ESR_COUNT];

	/* status variable */
	u32			*debug_mask;
	bool			force_shutdown;
	bool			qg_device_open;
	bool			profile_loaded;
	bool			battery_missing;
	bool			data_ready;
	bool			suspend_data;
	bool			vbat_low;
	bool			charge_done;
	bool			parallel_enabled;
	bool			usb_present;
	bool			dc_present;
	bool			charge_full;
	bool			force_soc;
	bool			fvss_active;
	bool			tcss_active;
	bool			fastcharge_mode_enabled;
	bool			shutdown_delay;
	bool			bass_active;
	int			charge_status;
	int			charge_type;
	int			chg_iterm_ma;
	int			next_wakeup_ms;
	int			esr_actual;
	int			esr_nominal;
	int			soh;
	int			soc_reporting_ready;
	int			last_fifo_v_uv;
	int			last_fifo_i_ua;
	int			prev_fifo_i_ua;
	int			soc_tcss_entry;
	int			ibat_tcss_entry;
	int			soc_tcss;
	int			tcss_entry_count;
	int			max_fcc_limit_ma;
	int			bsoc_bass_entry;
	u32			fifo_done_count;
	u32			wa_flags;
	u32			seq_no;
	u32			charge_counter_uah;
	u32			esr_avg;
	u32			esr_last;
	u32			s2_state;
	u32			s2_state_mask;
	u32			soc_fvss_entry;
	u32			vbat_fvss_entry;
	ktime_t			last_user_update_time;
	ktime_t			last_fifo_update_time;
	unsigned long		last_maint_soc_update_time;
	unsigned long		suspend_time;
	struct iio_channel	*batt_therm_chan;
	struct iio_channel	*batt_id_chan;

	/* soc params */
	int			catch_up_soc;
	int			maint_soc;
	int			msoc;
	int			pon_soc;
	int			batt_soc;
	int			cc_soc;
	int			full_soc;
	int			sys_soc;
	int			last_adj_ssoc;
	int			recharge_soc;
	int			batt_age_level;
	struct alarm		alarm_timer;
	u32			sdam_data[SDAM_MAX];
	int 			sdam_soc;

	/* DT */
	struct qg_dt		dt;
	struct qg_batt_props	bp;
	/* capacity learning */
	struct cap_learning	*cl;
	/* charge counter */
	struct cycle_counter	*counter;
	/* ttf */
	struct ttf		*ttf;

	/*battery temp compensation for F4 with diff ibat*/
	int			batt_ntc_comp;
	int			obj_temp;
	int			report_temp;
	int			last_ibat;
	int			real_temp;
	int			max_temp_comp_value;
	int			temp_comp_hysteresis;
	int			temp_comp_num;
	int			step_index;
	bool			temp_comp_cfg_valid;
	bool			temp_comp_enable;
	struct range_data	*temp_comp_cfg;

	int			batt_fake_temp;
};

struct ocv_all {
	u32 ocv_uv;
	u32 ocv_raw;
	char ocv_type[20];
};

enum ocv_type {
	S7_PON_OCV,
	S3_GOOD_OCV,
	S3_LAST_OCV,
	SDAM_PON_OCV,
	PON_OCV_MAX,
};

enum s2_state {
	S2_FAST_CHARGING = BIT(0),
	S2_LOW_VBAT = BIT(1),
	S2_SLEEP = BIT(2),
	S2_DEFAULT = BIT(3),
};

enum debug_mask {
	QG_DEBUG_PON		= BIT(0),
	QG_DEBUG_PROFILE	= BIT(1),
	QG_DEBUG_DEVICE		= BIT(2),
	QG_DEBUG_STATUS		= BIT(3),
	QG_DEBUG_FIFO		= BIT(4),
	QG_DEBUG_IRQ		= BIT(5),
	QG_DEBUG_SOC		= BIT(6),
	QG_DEBUG_PM		= BIT(7),
	QG_DEBUG_BUS_READ	= BIT(8),
	QG_DEBUG_BUS_WRITE	= BIT(9),
	QG_DEBUG_ALG_CL		= BIT(10),
	QG_DEBUG_ESR		= BIT(11),
};

enum qg_irq {
	QG_BATT_MISSING_IRQ,
	QG_VBATT_LOW_IRQ,
	QG_VBATT_EMPTY_IRQ,
	QG_FIFO_UPDATE_DONE_IRQ,
	QG_GOOD_OCV_IRQ,
	QG_FSM_STAT_CHG_IRQ,
	QG_EVENT_IRQ,
	QG_MAX_IRQ,
};

enum qg_wa_flags {
	QG_VBAT_LOW_WA = BIT(0),
	QG_RECHARGE_SOC_WA = BIT(1),
	QG_CLK_ADJUST_WA = BIT(2),
	QG_PON_OCV_WA = BIT(3),
};

enum batt_temp_comp {
	NTC_NO_COMP = 0,
	NTC_LOW_COMP = 2,
	NTC_MID_COMP = 4,
	NTC_HIGH_COMP = 6,
};
#endif /* __QG_CORE_H__ */
