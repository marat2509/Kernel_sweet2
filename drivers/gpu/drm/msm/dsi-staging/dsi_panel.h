/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
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
 *
 */

#ifndef _DSI_PANEL_H_
#define _DSI_PANEL_H_

#include <linux/of_device.h>
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/backlight.h>
#include <drm/drm_panel.h>
#include <drm/msm_drm.h>

#include "dsi_defs.h"
#include "dsi_ctrl_hw.h"
#include "dsi_clk.h"
#include "dsi_pwr.h"
#include "dsi_parser.h"
#include "msm_drv.h"

#define MAX_BL_LEVEL 4096
#define MAX_BL_SCALE_LEVEL 1024
#define MAX_AD_BL_SCALE_LEVEL 65535
#define DSI_CMD_PPS_SIZE 135

#define DSI_MODE_MAX 5

#define BUF_LEN_MAX    256

#define DEMURA_LEVEL_02 256
#define DEMURA_LEVEL_08 11
#define DEMURA_LEVEL_0D 1

enum dsi_panel_rotation {
	DSI_PANEL_ROTATE_NONE = 0,
	DSI_PANEL_ROTATE_HV_FLIP,
	DSI_PANEL_ROTATE_H_FLIP,
	DSI_PANEL_ROTATE_V_FLIP
};

enum dsi_backlight_type {
	DSI_BACKLIGHT_PWM = 0,
	DSI_BACKLIGHT_WLED,
	DSI_BACKLIGHT_DCS,
	DSI_BACKLIGHT_EXTERNAL,
	DSI_BACKLIGHT_UNKNOWN,
	DSI_BACKLIGHT_MAX,
};

enum bl_update_flag {
	BL_UPDATE_DELAY_UNTIL_FIRST_FRAME,
	BL_UPDATE_NONE,
};

enum {
	MODE_GPIO_NOT_VALID = 0,
	MODE_SEL_DUAL_PORT,
	MODE_SEL_SINGLE_PORT,
	MODE_GPIO_HIGH,
	MODE_GPIO_LOW,
};

enum dsi_dms_mode {
	DSI_DMS_MODE_DISABLED = 0,
	DSI_DMS_MODE_RES_SWITCH_IMMEDIATE,
};

enum dsi_panel_physical_type {
	DSI_DISPLAY_PANEL_TYPE_LCD = 0,
	DSI_DISPLAY_PANEL_TYPE_OLED,
	DSI_DISPLAY_PANEL_TYPE_MAX,
};

struct dsi_dfps_capabilities {
	enum dsi_dfps_type type;
	u32 min_refresh_rate;
	u32 max_refresh_rate;
	u32 *dfps_list;
	u32 dfps_list_len;
	bool dfps_support;
	/* smart fps control */
	bool smart_fps_support;
	u32 smart_fps_value;
};

struct dsi_dyn_clk_caps {
	bool dyn_clk_support;
	u32 *bit_clk_list;
	u32 bit_clk_list_len;
	bool skip_phy_timing_update;
	enum dsi_dyn_clk_feature_type type;
	bool maintain_const_fps;
};

struct dsi_pinctrl_info {
	struct pinctrl *pinctrl;
	struct pinctrl_state *active;
	struct pinctrl_state *suspend;
};

struct dsi_panel_phy_props {
	u32 panel_width_mm;
	u32 panel_height_mm;
	enum dsi_panel_rotation rotation;
};

struct dsi_backlight_config {
	enum dsi_backlight_type type;
	enum bl_update_flag bl_update;

	u32 bl_min_level;
	u32 bl_max_level;
	u32 brightness_max_level;
	u32 brightness_default_level;
	u32 bl_level;
	u32 bl_scale;
	u32 bl_scale_ad;
	bool bl_inverted_dbv;
	bool dcs_type_ss_ea;
	bool dcs_type_ss_eb;
	bool xiaomi_f4_36_flag;
	bool xiaomi_f4_41_flag;

	int en_gpio;
	bool bl_remap_flag;
	bool samsung_prepare_hbm_flag;
	/* PWM params */
	struct pwm_device *pwm_bl;
	bool pwm_enabled;
	u32 pwm_period_usecs;

	/* WLED params */
	struct led_trigger *wled;
	struct backlight_device *raw_bd;
};

struct dsi_reset_seq {
	u32 level;
	u32 sleep_ms;
};

struct dsi_panel_reset_config {
	struct dsi_reset_seq *sequence;
	u32 count;

	int reset_gpio;
	int disp_en_gpio;
	int lcd_mode_sel_gpio;
	u32 mode_sel_state;
};

enum esd_check_status_mode {
	ESD_MODE_REG_READ,
	ESD_MODE_SW_BTA,
	ESD_MODE_PANEL_TE,
	ESD_MODE_SW_SIM_SUCCESS,
	ESD_MODE_SW_SIM_FAILURE,
	ESD_MODE_MAX
};

struct drm_panel_esd_config {
	bool esd_enabled;

	enum esd_check_status_mode status_mode;
	struct dsi_panel_cmd_set offset_cmd;
	struct dsi_panel_cmd_set status_cmd;
	u32 *status_cmds_rlen;
	u32 *status_valid_params;
	u32 *status_value;
	u8 *return_buf;
	u8 *status_buf;
	u32 groups;
	int esd_err_irq_gpio;
	int esd_err_irq;
	int esd_err_irq_flags;
};

struct dsi_read_config {
	bool enabled;
	struct dsi_panel_cmd_set read_cmd;
	u32 cmds_rlen;
	u32 valid_bits;
	u8 rbuf[64];
};

struct dsi_panel {
	const char *name;
	const char *type;
	struct device_node *panel_of_node;
	struct mipi_dsi_device mipi_device;

	struct mutex panel_lock;
	struct drm_panel drm_panel;
	struct mipi_dsi_host *host;
	struct device *parent;

	struct dsi_host_common_cfg host_config;
	struct dsi_video_engine_cfg video_config;
	struct dsi_cmd_engine_cfg cmd_config;
	enum dsi_op_mode panel_mode;
	bool panel_mode_switch_enabled;

	struct dsi_dfps_capabilities dfps_caps;
	struct dsi_dyn_clk_caps dyn_clk_caps;
	struct dsi_panel_phy_props phy_props;

	struct dsi_display_mode *cur_mode;
	u32 num_timing_nodes;
	u32 num_display_modes;

	struct dsi_regulator_info power_info;
	struct dsi_backlight_config bl_config;
	struct dsi_panel_reset_config reset_config;
	struct dsi_pinctrl_info pinctrl;
	struct drm_panel_hdr_properties hdr_props;
	struct drm_panel_esd_config esd_config;

	struct dsi_parser_utils utils;

	bool lp11_init;
	bool ulps_feature_enabled;
	bool ulps_suspend_enabled;
	bool allow_phy_power_off;
	atomic_t esd_recovery_pending;

	bool panel_initialized;
	bool te_using_watchdog_timer;
	u32 qsync_min_fps;

	bool dispparam_enabled;
	u32 skip_dimmingon;

	char dsc_pps_cmd[DSI_CMD_PPS_SIZE];
	enum dsi_dms_mode dms_mode;

	bool sync_broadcast_en;
	int power_mode;
	enum dsi_panel_physical_type panel_type;

	u32 panel_on_dimming_delay;
	struct delayed_work cmds_work;
	struct delayed_work nolp_bl_delay_work;
	u32 last_bl_lvl;
	s32 backlight_delta;
	u32 backlight_demura_level; /* For the f4_41 panel */
	u32 backlight_pulse_threshold;
	/* DC bkl */
	u32 dc_demura_threshold;
	bool dc_enable;
	bool backlight_pulse_flag; /* true = 4 pulse and false = 1 pulse */
	u32 dc_threshold;
	bool k6_dc_flag;

	bool hbm_enabled;
	bool thermal_hbm_disabled;
	u32 hbm_brightness;
	bool fod_hbm_enabled;
	bool fod_dimlayer_enabled;
	bool fod_dimlayer_hbm_enabled;
	u32 fod_ui_ready;
	u32 doze_backlight_threshold;
	u32 fod_off_dimming_delay;
	ktime_t fod_backlight_off_time;
	ktime_t fod_hbm_off_time;
	bool f4_51_ctrl_flag; /* For the f4_36 panel */
	u32 hbm_ntfy_skip_flag;
	u32 hbm_off_51_index;
	u32 fod_off_51_index;

	bool elvss_dimming_check_enable;
	struct dsi_read_config elvss_dimming_cmds;
	struct dsi_panel_cmd_set elvss_dimming_offset;
	struct dsi_panel_cmd_set hbm_fod_on;
	struct dsi_panel_cmd_set hbm_fod_off;

	u8 panel_read_data[BUF_LEN_MAX];
	struct dsi_read_config xy_coordinate_cmds;

	bool fod_backlight_flag;
	bool fod_flag;
	u32 fod_target_backlight;
	bool fod_skip_flag; /* optimize to skip nolp command */
	bool in_aod; /* set  DISPPARAM_DOZE_BRIGHTNESS_HBM/LBM only in AOD */
	int doze_brightness;
	bool is_tddi_flag;
	bool panel_dead_flag;
	bool panel_max_frame_rate;

	bool nolp_command_set_backlight_enabled;
	bool oled_panel_video_mode;
	int doze_lbm_brightness;
	int doze_hbm_brightness;
 	int hbm_mode;
	bool resend_ea_hbm;
};

static inline bool dsi_panel_ulps_feature_enabled(struct dsi_panel *panel)
{
	return panel->ulps_feature_enabled;
}

static inline bool dsi_panel_initialized(struct dsi_panel *panel)
{
	return panel->panel_initialized;
}

static inline void dsi_panel_acquire_panel_lock(struct dsi_panel *panel)
{
	mutex_lock(&panel->panel_lock);
}

static inline void dsi_panel_release_panel_lock(struct dsi_panel *panel)
{
	mutex_unlock(&panel->panel_lock);
}

static inline bool dsi_panel_is_type_oled(struct dsi_panel *panel)
{
	return (panel->panel_type == DSI_DISPLAY_PANEL_TYPE_OLED);
}

struct dsi_panel *dsi_panel_get(struct device *parent,
				struct device_node *of_node,
				struct device_node *parser_node,
				const char *type,
				int topology_override);

int dsi_panel_trigger_esd_attack(struct dsi_panel *panel);

void dsi_panel_put(struct dsi_panel *panel);

int dsi_panel_drv_init(struct dsi_panel *panel, struct mipi_dsi_host *host);

int dsi_panel_drv_deinit(struct dsi_panel *panel);

int dsi_panel_get_mode_count(struct dsi_panel *panel);

void dsi_panel_put_mode(struct dsi_display_mode *mode);

int dsi_panel_get_mode(struct dsi_panel *panel,
		       u32 index,
		       struct dsi_display_mode *mode,
		       int topology_override);

int dsi_panel_validate_mode(struct dsi_panel *panel,
			    struct dsi_display_mode *mode);

int dsi_panel_get_host_cfg_for_mode(struct dsi_panel *panel,
				    struct dsi_display_mode *mode,
				    struct dsi_host_config *config);

int dsi_panel_get_phy_props(struct dsi_panel *panel,
			    struct dsi_panel_phy_props *phy_props);
int dsi_panel_get_dfps_caps(struct dsi_panel *panel,
			    struct dsi_dfps_capabilities *dfps_caps);

int dsi_panel_pre_prepare(struct dsi_panel *panel);

int dsi_panel_set_lp1(struct dsi_panel *panel);

int dsi_panel_set_lp2(struct dsi_panel *panel);

int dsi_panel_set_nolp(struct dsi_panel *panel);

int dsi_panel_prepare(struct dsi_panel *panel);

int dsi_panel_enable(struct dsi_panel *panel);

int dsi_panel_post_enable(struct dsi_panel *panel);

int dsi_panel_pre_disable(struct dsi_panel *panel);

int dsi_panel_disable(struct dsi_panel *panel);

int dsi_panel_unprepare(struct dsi_panel *panel);

int dsi_panel_post_unprepare(struct dsi_panel *panel);

int dsi_panel_set_backlight(struct dsi_panel *panel, u32 bl_lvl);

int dsi_panel_update_pps(struct dsi_panel *panel);

int dsi_panel_send_qsync_on_dcs(struct dsi_panel *panel,
		int ctrl_idx);
int dsi_panel_send_qsync_off_dcs(struct dsi_panel *panel,
		int ctrl_idx);

int dsi_panel_send_roi_dcs(struct dsi_panel *panel, int ctrl_idx,
		struct dsi_rect *roi);

int dsi_panel_pre_mode_switch_to_video(struct dsi_panel *panel);

int dsi_panel_pre_mode_switch_to_cmd(struct dsi_panel *panel);

int dsi_panel_mode_switch_to_cmd(struct dsi_panel *panel);

int dsi_panel_mode_switch_to_vid(struct dsi_panel *panel);

int dsi_panel_switch(struct dsi_panel *panel);

int dsi_panel_post_switch(struct dsi_panel *panel);

void dsi_dsc_pclk_param_calc(struct msm_display_dsc_info *dsc, int intf_width);

void dsi_panel_bl_handoff(struct dsi_panel *panel);

struct dsi_panel *dsi_panel_ext_bridge_get(struct device *parent,
				struct device_node *of_node,
				int topology_override);

int dsi_panel_parse_esd_reg_read_configs(struct dsi_panel *panel);

void dsi_panel_ext_bridge_put(struct dsi_panel *panel);

int dsi_panel_get_cmd_pkt_count(const char *data, u32 length, u32 *cnt);
int dsi_panel_alloc_cmd_packets(struct dsi_panel_cmd_set *cmd,
				u32 packet_count);
int dsi_panel_create_cmd_packets(const char *data,
				u32 length,
				u32 count,
				struct dsi_cmd_desc *cmd);
void dsi_panel_destroy_cmd_packets(struct dsi_panel_cmd_set *set);
void dsi_panel_dealloc_cmd_packets(struct dsi_panel_cmd_set *set);

int dsi_panel_write_cmd_set(struct dsi_panel *panel,
				struct dsi_panel_cmd_set *cmd_sets);

int dsi_panel_read_cmd_set(struct dsi_panel *panel,
				struct dsi_read_config *read_config);

ssize_t dsi_panel_mipi_reg_write(struct dsi_panel *panel,
				char *buf, size_t count);

ssize_t dsi_panel_mipi_reg_read(struct dsi_panel *panel,
				char *buf);

int dsi_panel_set_thermal_hbm_disabled(struct dsi_panel *panel,
				bool thermal_hbm_disabled);
int dsi_panel_get_thermal_hbm_disabled(struct dsi_panel *panel,
				bool *thermal_hbm_disabled);

int dsi_panel_apply_hbm_mode(struct dsi_panel *panel);

int dsi_panel_apply_dc_mode(struct dsi_panel *panel);

int dsi_panel_db_ic_enable(struct dsi_panel *panel);

int dsi_panel_mipi_reg_show(struct dsi_panel *panel);
#endif /* _DSI_PANEL_H_ */
