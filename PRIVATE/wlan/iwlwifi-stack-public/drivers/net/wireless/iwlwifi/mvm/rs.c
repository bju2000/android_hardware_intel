/******************************************************************************
 *
 * Copyright(c) 2005 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2014 Intel Mobile Communications GmbH
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 * The full GNU General Public License is included in this distribution in the
 * file called LICENSE.
 *
 * Contact Information:
 *  Intel Linux Wireless <ilw@linux.intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 *****************************************************************************/
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/mac80211.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>

#include <linux/workqueue.h>
#include "rs.h"
#include "fw-api.h"
#include "sta.h"
#include "iwl-op-mode.h"
#include "mvm.h"

#define RS_NAME "iwl-mvm-rs"

#define NUM_TRY_BEFORE_ANT_TOGGLE       1
#define RS_LEGACY_RETRIES_PER_RATE      1
#define RS_HT_VHT_RETRIES_PER_RATE      2
#define RS_HT_VHT_RETRIES_PER_RATE_TW   1
#define RS_INITIAL_MIMO_NUM_RATES       3
#define RS_INITIAL_SISO_NUM_RATES       3
#define RS_INITIAL_LEGACY_NUM_RATES     LINK_QUAL_MAX_RETRY_NUM
#define RS_SECONDARY_LEGACY_NUM_RATES   LINK_QUAL_MAX_RETRY_NUM
#define RS_SECONDARY_SISO_NUM_RATES     3
#define RS_SECONDARY_SISO_RETRIES       1

#define IWL_RATE_MAX_WINDOW		62	/* # tx in history window */
#define IWL_RATE_MIN_FAILURE_TH		3	/* min failures to calc tpt */
#define IWL_RATE_MIN_SUCCESS_TH		8	/* min successes to calc tpt */

/* max allowed rate miss before sync LQ cmd */
#define IWL_MISSED_RATE_MAX		15
#define RS_STAY_IN_COLUMN_TIMEOUT       (5*HZ)
#define RS_IDLE_TIMEOUT                 (5*HZ)

static u8 rs_ht_to_legacy[] = {
	[IWL_RATE_MCS_0_INDEX] = IWL_RATE_6M_INDEX,
	[IWL_RATE_MCS_1_INDEX] = IWL_RATE_9M_INDEX,
	[IWL_RATE_MCS_2_INDEX] = IWL_RATE_12M_INDEX,
	[IWL_RATE_MCS_3_INDEX] = IWL_RATE_18M_INDEX,
	[IWL_RATE_MCS_4_INDEX] = IWL_RATE_24M_INDEX,
	[IWL_RATE_MCS_5_INDEX] = IWL_RATE_36M_INDEX,
	[IWL_RATE_MCS_6_INDEX] = IWL_RATE_48M_INDEX,
	[IWL_RATE_MCS_7_INDEX] = IWL_RATE_54M_INDEX,
	[IWL_RATE_MCS_8_INDEX] = IWL_RATE_54M_INDEX,
	[IWL_RATE_MCS_9_INDEX] = IWL_RATE_54M_INDEX,
};

static const u8 ant_toggle_lookup[] = {
	[ANT_NONE] = ANT_NONE,
	[ANT_A] = ANT_B,
	[ANT_B] = ANT_C,
	[ANT_AB] = ANT_BC,
	[ANT_C] = ANT_A,
	[ANT_AC] = ANT_AB,
	[ANT_BC] = ANT_AC,
	[ANT_ABC] = ANT_ABC,
};

#define IWL_DECLARE_RATE_INFO(r, s, rp, rn)			      \
	[IWL_RATE_##r##M_INDEX] = { IWL_RATE_##r##M_PLCP,	      \
				    IWL_RATE_HT_SISO_MCS_##s##_PLCP,  \
				    IWL_RATE_HT_MIMO2_MCS_##s##_PLCP, \
				    IWL_RATE_VHT_SISO_MCS_##s##_PLCP, \
				    IWL_RATE_VHT_MIMO2_MCS_##s##_PLCP,\
				    IWL_RATE_##rp##M_INDEX,	      \
				    IWL_RATE_##rn##M_INDEX }

#define IWL_DECLARE_MCS_RATE(s)						  \
	[IWL_RATE_MCS_##s##_INDEX] = { IWL_RATE_INVM_PLCP,		  \
				       IWL_RATE_HT_SISO_MCS_##s##_PLCP,	  \
				       IWL_RATE_HT_MIMO2_MCS_##s##_PLCP,  \
				       IWL_RATE_VHT_SISO_MCS_##s##_PLCP,  \
				       IWL_RATE_VHT_MIMO2_MCS_##s##_PLCP, \
				       IWL_RATE_INVM_INDEX,	          \
				       IWL_RATE_INVM_INDEX }

/*
 * Parameter order:
 *   rate, ht rate, prev rate, next rate
 *
 * If there isn't a valid next or previous rate then INV is used which
 * maps to IWL_RATE_INVALID
 *
 */
static const struct iwl_rs_rate_info iwl_rates[IWL_RATE_COUNT] = {
	IWL_DECLARE_RATE_INFO(1, INV, INV, 2),   /*  1mbps */
	IWL_DECLARE_RATE_INFO(2, INV, 1, 5),     /*  2mbps */
	IWL_DECLARE_RATE_INFO(5, INV, 2, 11),    /*5.5mbps */
	IWL_DECLARE_RATE_INFO(11, INV, 9, 12),   /* 11mbps */
	IWL_DECLARE_RATE_INFO(6, 0, 5, 11),      /*  6mbps ; MCS 0 */
	IWL_DECLARE_RATE_INFO(9, INV, 6, 11),    /*  9mbps */
	IWL_DECLARE_RATE_INFO(12, 1, 11, 18),    /* 12mbps ; MCS 1 */
	IWL_DECLARE_RATE_INFO(18, 2, 12, 24),    /* 18mbps ; MCS 2 */
	IWL_DECLARE_RATE_INFO(24, 3, 18, 36),    /* 24mbps ; MCS 3 */
	IWL_DECLARE_RATE_INFO(36, 4, 24, 48),    /* 36mbps ; MCS 4 */
	IWL_DECLARE_RATE_INFO(48, 5, 36, 54),    /* 48mbps ; MCS 5 */
	IWL_DECLARE_RATE_INFO(54, 6, 48, INV),   /* 54mbps ; MCS 6 */
	IWL_DECLARE_MCS_RATE(7),                 /* MCS 7 */
	IWL_DECLARE_MCS_RATE(8),                 /* MCS 8 */
	IWL_DECLARE_MCS_RATE(9),                 /* MCS 9 */
};

enum rs_action {
	RS_ACTION_STAY = 0,
	RS_ACTION_DOWNSCALE = -1,
	RS_ACTION_UPSCALE = 1,
};

enum rs_column_mode {
	RS_INVALID = 0,
	RS_LEGACY,
	RS_SISO,
	RS_MIMO2,
};

#define MAX_NEXT_COLUMNS 7
#define MAX_COLUMN_CHECKS 3

typedef bool (*allow_column_func_t) (struct iwl_mvm *mvm,
				     struct ieee80211_sta *sta,
				     struct iwl_scale_tbl_info *tbl);

struct rs_tx_column {
	enum rs_column_mode mode;
	u8 ant;
	bool sgi;
	enum rs_column next_columns[MAX_NEXT_COLUMNS];
	allow_column_func_t checks[MAX_COLUMN_CHECKS];
};

static bool rs_mimo_allow(struct iwl_mvm *mvm, struct ieee80211_sta *sta,
			  struct iwl_scale_tbl_info *tbl)
{
	if (!sta->ht_cap.ht_supported)
		return false;

	if (sta->smps_mode == IEEE80211_SMPS_STATIC)
		return false;

	if (num_of_ant(mvm->fw->valid_tx_ant) < 2)
		return false;

	if (!iwl_mvm_bt_coex_is_mimo_allowed(mvm, sta))
		return false;

	return true;
}

static bool rs_siso_allow(struct iwl_mvm *mvm, struct ieee80211_sta *sta,
			  struct iwl_scale_tbl_info *tbl)
{
	if (!sta->ht_cap.ht_supported)
		return false;

	return true;
}

static bool rs_sgi_allow(struct iwl_mvm *mvm, struct ieee80211_sta *sta,
			 struct iwl_scale_tbl_info *tbl)
{
	struct rs_rate *rate = &tbl->rate;
	struct ieee80211_sta_ht_cap *ht_cap = &sta->ht_cap;
	struct ieee80211_sta_vht_cap *vht_cap = &sta->vht_cap;

	if (is_ht20(rate) && (ht_cap->cap &
			     IEEE80211_HT_CAP_SGI_20))
		return true;
	if (is_ht40(rate) && (ht_cap->cap &
			     IEEE80211_HT_CAP_SGI_40))
		return true;
	if (is_ht80(rate) && (vht_cap->cap &
			     IEEE80211_VHT_CAP_SHORT_GI_80))
		return true;

	return false;
}

static const struct rs_tx_column rs_tx_columns[] = {
	[RS_COLUMN_LEGACY_ANT_A] = {
		.mode = RS_LEGACY,
		.ant = ANT_A,
		.next_columns = {
			RS_COLUMN_LEGACY_ANT_B,
			RS_COLUMN_SISO_ANT_A,
			RS_COLUMN_MIMO2,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
		},
	},
	[RS_COLUMN_LEGACY_ANT_B] = {
		.mode = RS_LEGACY,
		.ant = ANT_B,
		.next_columns = {
			RS_COLUMN_LEGACY_ANT_A,
			RS_COLUMN_SISO_ANT_B,
			RS_COLUMN_MIMO2,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
		},
	},
	[RS_COLUMN_SISO_ANT_A] = {
		.mode = RS_SISO,
		.ant = ANT_A,
		.next_columns = {
			RS_COLUMN_SISO_ANT_B,
			RS_COLUMN_MIMO2,
			RS_COLUMN_SISO_ANT_A_SGI,
			RS_COLUMN_LEGACY_ANT_A,
			RS_COLUMN_LEGACY_ANT_B,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
		},
		.checks = {
			rs_siso_allow,
		},
	},
	[RS_COLUMN_SISO_ANT_B] = {
		.mode = RS_SISO,
		.ant = ANT_B,
		.next_columns = {
			RS_COLUMN_SISO_ANT_A,
			RS_COLUMN_MIMO2,
			RS_COLUMN_SISO_ANT_B_SGI,
			RS_COLUMN_LEGACY_ANT_A,
			RS_COLUMN_LEGACY_ANT_B,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
		},
		.checks = {
			rs_siso_allow,
		},
	},
	[RS_COLUMN_SISO_ANT_A_SGI] = {
		.mode = RS_SISO,
		.ant = ANT_A,
		.sgi = true,
		.next_columns = {
			RS_COLUMN_SISO_ANT_B_SGI,
			RS_COLUMN_MIMO2_SGI,
			RS_COLUMN_SISO_ANT_A,
			RS_COLUMN_LEGACY_ANT_A,
			RS_COLUMN_LEGACY_ANT_B,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
		},
		.checks = {
			rs_siso_allow,
			rs_sgi_allow,
		},
	},
	[RS_COLUMN_SISO_ANT_B_SGI] = {
		.mode = RS_SISO,
		.ant = ANT_B,
		.sgi = true,
		.next_columns = {
			RS_COLUMN_SISO_ANT_A_SGI,
			RS_COLUMN_MIMO2_SGI,
			RS_COLUMN_SISO_ANT_B,
			RS_COLUMN_LEGACY_ANT_A,
			RS_COLUMN_LEGACY_ANT_B,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
		},
		.checks = {
			rs_siso_allow,
			rs_sgi_allow,
		},
	},
	[RS_COLUMN_MIMO2] = {
		.mode = RS_MIMO2,
		.ant = ANT_AB,
		.next_columns = {
			RS_COLUMN_SISO_ANT_A,
			RS_COLUMN_MIMO2_SGI,
			RS_COLUMN_LEGACY_ANT_A,
			RS_COLUMN_LEGACY_ANT_B,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
		},
		.checks = {
			rs_mimo_allow,
		},
	},
	[RS_COLUMN_MIMO2_SGI] = {
		.mode = RS_MIMO2,
		.ant = ANT_AB,
		.sgi = true,
		.next_columns = {
			RS_COLUMN_SISO_ANT_A_SGI,
			RS_COLUMN_MIMO2,
			RS_COLUMN_LEGACY_ANT_A,
			RS_COLUMN_LEGACY_ANT_B,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
			RS_COLUMN_INVALID,
		},
		.checks = {
			rs_mimo_allow,
			rs_sgi_allow,
		},
	},
};

static inline u8 rs_extract_rate(u32 rate_n_flags)
{
	/* also works for HT because bits 7:6 are zero there */
	return (u8)(rate_n_flags & RATE_LEGACY_RATE_MSK);
}

static int iwl_hwrate_to_plcp_idx(u32 rate_n_flags)
{
	int idx = 0;

	if (rate_n_flags & RATE_MCS_HT_MSK) {
		idx = rate_n_flags & RATE_HT_MCS_RATE_CODE_MSK;
		idx += IWL_RATE_MCS_0_INDEX;

		/* skip 9M not supported in HT*/
		if (idx >= IWL_RATE_9M_INDEX)
			idx += 1;
		if ((idx >= IWL_FIRST_HT_RATE) && (idx <= IWL_LAST_HT_RATE))
			return idx;
	} else if (rate_n_flags & RATE_MCS_VHT_MSK) {
		idx = rate_n_flags & RATE_VHT_MCS_RATE_CODE_MSK;
		idx += IWL_RATE_MCS_0_INDEX;

		/* skip 9M not supported in VHT*/
		if (idx >= IWL_RATE_9M_INDEX)
			idx++;
		if ((idx >= IWL_FIRST_VHT_RATE) && (idx <= IWL_LAST_VHT_RATE))
			return idx;
	} else {
		/* legacy rate format, search for match in table */

		u8 legacy_rate = rs_extract_rate(rate_n_flags);
		for (idx = 0; idx < ARRAY_SIZE(iwl_rates); idx++)
			if (iwl_rates[idx].plcp == legacy_rate)
				return idx;
	}

	return IWL_RATE_INVALID;
}

static void rs_rate_scale_perform(struct iwl_mvm *mvm,
				   struct sk_buff *skb,
				   struct ieee80211_sta *sta,
				   struct iwl_lq_sta *lq_sta);
static void rs_fill_lq_cmd(struct iwl_mvm *mvm,
			   struct ieee80211_sta *sta,
			   struct iwl_lq_sta *lq_sta,
			   const struct rs_rate *initial_rate);
static void rs_stay_in_table(struct iwl_lq_sta *lq_sta, bool force_search);

/**
 * The following tables contain the expected throughput metrics for all rates
 *
 *	1, 2, 5.5, 11, 6, 9, 12, 18, 24, 36, 48, 54, 60 MBits
 *
 * where invalid entries are zeros.
 *
 * CCK rates are only valid in legacy table and will only be used in G
 * (2.4 GHz) band.
 */

static const u16 expected_tpt_legacy[IWL_RATE_COUNT] = {
	7, 13, 35, 58, 40, 57, 72, 98, 121, 154, 177, 186, 0, 0, 0
};

/* Expected TpT tables. 4 indexes:
 * 0 - NGI, 1 - SGI, 2 - AGG+NGI, 3 - AGG+SGI
 */
static const u16 expected_tpt_siso_20MHz[4][IWL_RATE_COUNT] = {
	{0, 0, 0, 0, 42, 0,  76, 102, 124, 159, 183, 193, 202, 216, 0},
	{0, 0, 0, 0, 46, 0,  82, 110, 132, 168, 192, 202, 210, 225, 0},
	{0, 0, 0, 0, 49, 0,  97, 145, 192, 285, 375, 420, 464, 551, 0},
	{0, 0, 0, 0, 54, 0, 108, 160, 213, 315, 415, 465, 513, 608, 0},
};

static const u16 expected_tpt_siso_40MHz[4][IWL_RATE_COUNT] = {
	{0, 0, 0, 0,  77, 0, 127, 160, 184, 220, 242, 250,  257,  269,  275},
	{0, 0, 0, 0,  83, 0, 135, 169, 193, 229, 250, 257,  264,  275,  280},
	{0, 0, 0, 0, 101, 0, 199, 295, 389, 570, 744, 828,  911, 1070, 1173},
	{0, 0, 0, 0, 112, 0, 220, 326, 429, 629, 819, 912, 1000, 1173, 1284},
};

static const u16 expected_tpt_siso_80MHz[4][IWL_RATE_COUNT] = {
	{0, 0, 0, 0, 130, 0, 191, 223, 244,  273,  288,  294,  298,  305,  308},
	{0, 0, 0, 0, 138, 0, 200, 231, 251,  279,  293,  298,  302,  308,  312},
	{0, 0, 0, 0, 217, 0, 429, 634, 834, 1220, 1585, 1760, 1931, 2258, 2466},
	{0, 0, 0, 0, 241, 0, 475, 701, 921, 1343, 1741, 1931, 2117, 2468, 2691},
};

static const u16 expected_tpt_mimo2_20MHz[4][IWL_RATE_COUNT] = {
	{0, 0, 0, 0,  74, 0, 123, 155, 179, 213, 235, 243, 250,  261, 0},
	{0, 0, 0, 0,  81, 0, 131, 164, 187, 221, 242, 250, 256,  267, 0},
	{0, 0, 0, 0,  98, 0, 193, 286, 375, 550, 718, 799, 878, 1032, 0},
	{0, 0, 0, 0, 109, 0, 214, 316, 414, 607, 790, 879, 965, 1132, 0},
};

static const u16 expected_tpt_mimo2_40MHz[4][IWL_RATE_COUNT] = {
	{0, 0, 0, 0, 123, 0, 182, 214, 235,  264,  279,  285,  289,  296,  300},
	{0, 0, 0, 0, 131, 0, 191, 222, 242,  270,  284,  289,  293,  300,  303},
	{0, 0, 0, 0, 200, 0, 390, 571, 741, 1067, 1365, 1505, 1640, 1894, 2053},
	{0, 0, 0, 0, 221, 0, 430, 630, 816, 1169, 1490, 1641, 1784, 2053, 2221},
};

static const u16 expected_tpt_mimo2_80MHz[4][IWL_RATE_COUNT] = {
	{0, 0, 0, 0, 182, 0, 240,  264,  278,  299,  308,  311,  313,  317,  319},
	{0, 0, 0, 0, 190, 0, 247,  269,  282,  302,  310,  313,  315,  319,  320},
	{0, 0, 0, 0, 428, 0, 833, 1215, 1577, 2254, 2863, 3147, 3418, 3913, 4219},
	{0, 0, 0, 0, 474, 0, 920, 1338, 1732, 2464, 3116, 3418, 3705, 4225, 4545},
};

/* mbps, mcs */
static const struct iwl_rate_mcs_info iwl_rate_mcs[IWL_RATE_COUNT] = {
	{  "1", "BPSK DSSS"},
	{  "2", "QPSK DSSS"},
	{"5.5", "BPSK CCK"},
	{ "11", "QPSK CCK"},
	{  "6", "BPSK 1/2"},
	{  "9", "BPSK 1/2"},
	{ "12", "QPSK 1/2"},
	{ "18", "QPSK 3/4"},
	{ "24", "16QAM 1/2"},
	{ "36", "16QAM 3/4"},
	{ "48", "64QAM 2/3"},
	{ "54", "64QAM 3/4"},
	{ "60", "64QAM 5/6"},
};

#define MCS_INDEX_PER_STREAM	(8)

static const char *rs_pretty_ant(u8 ant)
{
	static const char * const ant_name[] = {
		[ANT_NONE] = "None",
		[ANT_A]    = "A",
		[ANT_B]    = "B",
		[ANT_AB]   = "AB",
		[ANT_C]    = "C",
		[ANT_AC]   = "AC",
		[ANT_BC]   = "BC",
		[ANT_ABC]  = "ABC",
	};

	if (ant > ANT_ABC)
		return "UNKNOWN";

	return ant_name[ant];
}

static const char *rs_pretty_lq_type(enum iwl_table_type type)
{
	static const char * const lq_types[] = {
		[LQ_NONE] = "NONE",
		[LQ_LEGACY_A] = "LEGACY_A",
		[LQ_LEGACY_G] = "LEGACY_G",
		[LQ_HT_SISO] = "HT SISO",
		[LQ_HT_MIMO2] = "HT MIMO",
		[LQ_VHT_SISO] = "VHT SISO",
		[LQ_VHT_MIMO2] = "VHT MIMO",
	};

	if (type < LQ_NONE || type >= LQ_MAX)
		return "UNKNOWN";

	return lq_types[type];
}

static inline void rs_dump_rate(struct iwl_mvm *mvm, const struct rs_rate *rate,
				const char *prefix)
{
	IWL_DEBUG_RATE(mvm, "%s: (%s: %d) ANT: %s BW: %d SGI: %d LDPC: %d\n",
		       prefix, rs_pretty_lq_type(rate->type),
		       rate->index, rs_pretty_ant(rate->ant),
		       rate->bw, rate->sgi, rate->ldpc);
}

static void rs_rate_scale_clear_window(struct iwl_rate_scale_data *window)
{
	window->data = 0;
	window->success_counter = 0;
	window->success_ratio = IWL_INVALID_VALUE;
	window->counter = 0;
	window->average_tpt = IWL_INVALID_VALUE;
}

static void rs_rate_scale_clear_tbl_windows(struct iwl_mvm *mvm,
					    struct iwl_scale_tbl_info *tbl)
{
	int i;

	IWL_DEBUG_RATE(mvm, "Clearing up window stats\n");
	for (i = 0; i < IWL_RATE_COUNT; i++)
		rs_rate_scale_clear_window(&tbl->win[i]);

	for (i = 0; i < ARRAY_SIZE(tbl->tpc_win); i++)
		rs_rate_scale_clear_window(&tbl->tpc_win[i]);
}

static inline u8 rs_is_valid_ant(u8 valid_antenna, u8 ant_type)
{
	return (ant_type & valid_antenna) == ant_type;
}

static int rs_tl_turn_on_agg_for_tid(struct iwl_mvm *mvm,
				      struct iwl_lq_sta *lq_data, u8 tid,
				      struct ieee80211_sta *sta)
{
	int ret = -EAGAIN;

	IWL_DEBUG_HT(mvm, "Starting Tx agg: STA: %pM tid: %d\n",
		     sta->addr, tid);
	ret = ieee80211_start_tx_ba_session(sta, tid, 5000);
	if (ret == -EAGAIN) {
		/*
		 * driver and mac80211 is out of sync
		 * this might be cause by reloading firmware
		 * stop the tx ba session here
		 */
		IWL_ERR(mvm, "Fail start Tx agg on tid: %d\n",
			tid);
		ieee80211_stop_tx_ba_session(sta, tid);
	}
	return ret;
}

static void rs_tl_turn_on_agg(struct iwl_mvm *mvm, u8 tid,
			      struct iwl_lq_sta *lq_data,
			      struct ieee80211_sta *sta)
{
	if (tid < IWL_MAX_TID_COUNT)
		rs_tl_turn_on_agg_for_tid(mvm, lq_data, tid, sta);
	else
		IWL_ERR(mvm, "tid exceeds max TID count: %d/%d\n",
			tid, IWL_MAX_TID_COUNT);
}

static inline int get_num_of_ant_from_rate(u32 rate_n_flags)
{
	return !!(rate_n_flags & RATE_MCS_ANT_A_MSK) +
	       !!(rate_n_flags & RATE_MCS_ANT_B_MSK) +
	       !!(rate_n_flags & RATE_MCS_ANT_C_MSK);
}

/*
 * Static function to get the expected throughput from an iwl_scale_tbl_info
 * that wraps a NULL pointer check
 */
static s32 get_expected_tpt(struct iwl_scale_tbl_info *tbl, int rs_index)
{
	if (tbl->expected_tpt)
		return tbl->expected_tpt[rs_index];
	return 0;
}

/**
 * rs_collect_tx_data - Update the success/failure sliding window
 *
 * We keep a sliding window of the last 62 packets transmitted
 * at this rate.  window->data contains the bitmask of successful
 * packets.
 */
static int _rs_collect_tx_data(struct iwl_scale_tbl_info *tbl,
			       int scale_index, int attempts, int successes,
			       struct iwl_rate_scale_data *window)
{
	static const u64 mask = (((u64)1) << (IWL_RATE_MAX_WINDOW - 1));
	s32 fail_count, tpt;

	/* Get expected throughput */
	tpt = get_expected_tpt(tbl, scale_index);

	/*
	 * Keep track of only the latest 62 tx frame attempts in this rate's
	 * history window; anything older isn't really relevant any more.
	 * If we have filled up the sliding window, drop the oldest attempt;
	 * if the oldest attempt (highest bit in bitmap) shows "success",
	 * subtract "1" from the success counter (this is the main reason
	 * we keep these bitmaps!).
	 */
	while (attempts > 0) {
		if (window->counter >= IWL_RATE_MAX_WINDOW) {
			/* remove earliest */
			window->counter = IWL_RATE_MAX_WINDOW - 1;

			if (window->data & mask) {
				window->data &= ~mask;
				window->success_counter--;
			}
		}

		/* Increment frames-attempted counter */
		window->counter++;

		/* Shift bitmap by one frame to throw away oldest history */
		window->data <<= 1;

		/* Mark the most recent #successes attempts as successful */
		if (successes > 0) {
			window->success_counter++;
			window->data |= 0x1;
			successes--;
		}

		attempts--;
	}

	/* Calculate current success ratio, avoid divide-by-0! */
	if (window->counter > 0)
		window->success_ratio = 128 * (100 * window->success_counter)
					/ window->counter;
	else
		window->success_ratio = IWL_INVALID_VALUE;

	fail_count = window->counter - window->success_counter;

	/* Calculate average throughput, if we have enough history. */
	if ((fail_count >= IWL_RATE_MIN_FAILURE_TH) ||
	    (window->success_counter >= IWL_RATE_MIN_SUCCESS_TH))
		window->average_tpt = (window->success_ratio * tpt + 64) / 128;
	else
		window->average_tpt = IWL_INVALID_VALUE;

	return 0;
}

static int rs_collect_tx_data(struct iwl_lq_sta *lq_sta,
			      struct iwl_scale_tbl_info *tbl,
			      int scale_index, int attempts, int successes,
			      u8 reduced_txp)
{
	struct iwl_rate_scale_data *window = NULL;
	int ret;

	if (scale_index < 0 || scale_index >= IWL_RATE_COUNT)
		return -EINVAL;

	if (tbl->column != RS_COLUMN_INVALID) {
		lq_sta->tx_stats[tbl->column][scale_index].total += attempts;
		lq_sta->tx_stats[tbl->column][scale_index].success += successes;
	}

	/* Select window for current tx bit rate */
	window = &(tbl->win[scale_index]);

	ret = _rs_collect_tx_data(tbl, scale_index, attempts, successes,
				  window);
	if (ret)
		return ret;

	if (WARN_ON_ONCE(reduced_txp > TPC_MAX_REDUCTION))
		return -EINVAL;

	window = &tbl->tpc_win[reduced_txp];
	return _rs_collect_tx_data(tbl, scale_index, attempts, successes,
				   window);
}

/* Convert rs_rate object into ucode rate bitmask */
static u32 ucode_rate_from_rs_rate(struct iwl_mvm *mvm,
				  struct rs_rate *rate)
{
	u32 ucode_rate = 0;
	int index = rate->index;

	ucode_rate |= ((rate->ant << RATE_MCS_ANT_POS) &
			 RATE_MCS_ANT_ABC_MSK);

	if (is_legacy(rate)) {
		ucode_rate |= iwl_rates[index].plcp;
		if (index >= IWL_FIRST_CCK_RATE && index <= IWL_LAST_CCK_RATE)
			ucode_rate |= RATE_MCS_CCK_MSK;
		return ucode_rate;
	}

	if (is_ht(rate)) {
		if (index < IWL_FIRST_HT_RATE || index > IWL_LAST_HT_RATE) {
			IWL_ERR(mvm, "Invalid HT rate index %d\n", index);
			index = IWL_LAST_HT_RATE;
		}
		ucode_rate |= RATE_MCS_HT_MSK;

		if (is_ht_siso(rate))
			ucode_rate |= iwl_rates[index].plcp_ht_siso;
		else if (is_ht_mimo2(rate))
			ucode_rate |= iwl_rates[index].plcp_ht_mimo2;
		else
			WARN_ON_ONCE(1);
	} else if (is_vht(rate)) {
		if (index < IWL_FIRST_VHT_RATE || index > IWL_LAST_VHT_RATE) {
			IWL_ERR(mvm, "Invalid VHT rate index %d\n", index);
			index = IWL_LAST_VHT_RATE;
		}
		ucode_rate |= RATE_MCS_VHT_MSK;
		if (is_vht_siso(rate))
			ucode_rate |= iwl_rates[index].plcp_vht_siso;
		else if (is_vht_mimo2(rate))
			ucode_rate |= iwl_rates[index].plcp_vht_mimo2;
		else
			WARN_ON_ONCE(1);

	} else {
		IWL_ERR(mvm, "Invalid rate->type %d\n", rate->type);
	}

	ucode_rate |= rate->bw;
	if (rate->sgi)
		ucode_rate |= RATE_MCS_SGI_MSK;
	if (rate->ldpc)
		ucode_rate |= RATE_MCS_LDPC_MSK;

	return ucode_rate;
}

/* Convert a ucode rate into an rs_rate object */
static int rs_rate_from_ucode_rate(const u32 ucode_rate,
				   enum ieee80211_band band,
				   struct rs_rate *rate)
{
	u32 ant_msk = ucode_rate & RATE_MCS_ANT_ABC_MSK;
	u8 num_of_ant = get_num_of_ant_from_rate(ucode_rate);
	u8 nss;

	memset(rate, 0, sizeof(*rate));
	rate->index = iwl_hwrate_to_plcp_idx(ucode_rate);

	if (rate->index == IWL_RATE_INVALID)
		return -EINVAL;

	rate->ant = (ant_msk >> RATE_MCS_ANT_POS);

	/* Legacy */
	if (!(ucode_rate & RATE_MCS_HT_MSK) &&
	    !(ucode_rate & RATE_MCS_VHT_MSK)) {
		if (num_of_ant == 1) {
			if (band == IEEE80211_BAND_5GHZ)
				rate->type = LQ_LEGACY_A;
			else
				rate->type = LQ_LEGACY_G;
		}

		return 0;
	}

	/* HT or VHT */
	if (ucode_rate & RATE_MCS_SGI_MSK)
		rate->sgi = true;
	if (ucode_rate & RATE_MCS_LDPC_MSK)
		rate->ldpc = true;

	rate->bw = ucode_rate & RATE_MCS_CHAN_WIDTH_MSK;

	if (ucode_rate & RATE_MCS_HT_MSK) {
		nss = ((ucode_rate & RATE_HT_MCS_NSS_MSK) >>
		       RATE_HT_MCS_NSS_POS) + 1;

		if (nss == 1) {
			rate->type = LQ_HT_SISO;
			WARN_ON_ONCE(num_of_ant != 1);
		} else if (nss == 2) {
			rate->type = LQ_HT_MIMO2;
			WARN_ON_ONCE(num_of_ant != 2);
		} else {
			WARN_ON_ONCE(1);
		}
	} else if (ucode_rate & RATE_MCS_VHT_MSK) {
		nss = ((ucode_rate & RATE_VHT_MCS_NSS_MSK) >>
		       RATE_VHT_MCS_NSS_POS) + 1;

		if (nss == 1) {
			rate->type = LQ_VHT_SISO;
			WARN_ON_ONCE(num_of_ant != 1);
		} else if (nss == 2) {
			rate->type = LQ_VHT_MIMO2;
			WARN_ON_ONCE(num_of_ant != 2);
		} else {
			WARN_ON_ONCE(1);
		}
	}

	WARN_ON_ONCE(rate->bw == RATE_MCS_CHAN_WIDTH_160);
	WARN_ON_ONCE(rate->bw == RATE_MCS_CHAN_WIDTH_80 &&
		     !is_vht(rate));

	return 0;
}

/* switch to another antenna/antennas and return 1 */
/* if no other valid antenna found, return 0 */
static int rs_toggle_antenna(u32 valid_ant, struct rs_rate *rate)
{
	u8 new_ant_type;

	if (!rate->ant || rate->ant > ANT_ABC)
		return 0;

	if (!rs_is_valid_ant(valid_ant, rate->ant))
		return 0;

	new_ant_type = ant_toggle_lookup[rate->ant];

	while ((new_ant_type != rate->ant) &&
	       !rs_is_valid_ant(valid_ant, new_ant_type))
		new_ant_type = ant_toggle_lookup[new_ant_type];

	if (new_ant_type == rate->ant)
		return 0;

	rate->ant = new_ant_type;

	return 1;
}

static u16 rs_get_supported_rates(struct iwl_lq_sta *lq_sta,
				  struct rs_rate *rate)
{
	if (is_legacy(rate))
		return lq_sta->active_legacy_rate;
	else if (is_siso(rate))
		return lq_sta->active_siso_rate;
	else if (is_mimo2(rate))
		return lq_sta->active_mimo2_rate;

	WARN_ON_ONCE(1);
	return 0;
}

static u16 rs_get_adjacent_rate(struct iwl_mvm *mvm, u8 index, u16 rate_mask,
				int rate_type)
{
	u8 high = IWL_RATE_INVALID;
	u8 low = IWL_RATE_INVALID;

	/* 802.11A or ht walks to the next literal adjacent rate in
	 * the rate table */
	if (is_type_a_band(rate_type) || !is_type_legacy(rate_type)) {
		int i;
		u32 mask;

		/* Find the previous rate that is in the rate mask */
		i = index - 1;
		for (mask = (1 << i); i >= 0; i--, mask >>= 1) {
			if (rate_mask & mask) {
				low = i;
				break;
			}
		}

		/* Find the next rate that is in the rate mask */
		i = index + 1;
		for (mask = (1 << i); i < IWL_RATE_COUNT; i++, mask <<= 1) {
			if (rate_mask & mask) {
				high = i;
				break;
			}
		}

		return (high << 8) | low;
	}

	low = index;
	while (low != IWL_RATE_INVALID) {
		low = iwl_rates[low].prev_rs;
		if (low == IWL_RATE_INVALID)
			break;
		if (rate_mask & (1 << low))
			break;
		IWL_DEBUG_RATE(mvm, "Skipping masked lower rate: %d\n", low);
	}

	high = index;
	while (high != IWL_RATE_INVALID) {
		high = iwl_rates[high].next_rs;
		if (high == IWL_RATE_INVALID)
			break;
		if (rate_mask & (1 << high))
			break;
		IWL_DEBUG_RATE(mvm, "Skipping masked higher rate: %d\n", high);
	}

	return (high << 8) | low;
}

static inline bool rs_rate_supported(struct iwl_lq_sta *lq_sta,
				     struct rs_rate *rate)
{
	return BIT(rate->index) & rs_get_supported_rates(lq_sta, rate);
}

/* Get the next supported lower rate in the current column.
 * Return true if bottom rate in the current column was reached
 */
static bool rs_get_lower_rate_in_column(struct iwl_lq_sta *lq_sta,
					struct rs_rate *rate)
{
	u8 low;
	u16 high_low;
	u16 rate_mask;
	struct iwl_mvm *mvm = lq_sta->pers.drv;

	rate_mask = rs_get_supported_rates(lq_sta, rate);
	high_low = rs_get_adjacent_rate(mvm, rate->index, rate_mask,
					rate->type);
	low = high_low & 0xff;

	/* Bottom rate of column reached */
	if (low == IWL_RATE_INVALID)
		return true;

	rate->index = low;
	return false;
}

/* Get the next rate to use following a column downgrade */
static void rs_get_lower_rate_down_column(struct iwl_lq_sta *lq_sta,
					  struct rs_rate *rate)
{
	struct iwl_mvm *mvm = lq_sta->pers.drv;

	if (is_legacy(rate)) {
		/* No column to downgrade from Legacy */
		return;
	} else if (is_siso(rate)) {
		/* Downgrade to Legacy if we were in SISO */
		if (lq_sta->band == IEEE80211_BAND_5GHZ)
			rate->type = LQ_LEGACY_A;
		else
			rate->type = LQ_LEGACY_G;

		rate->bw = RATE_MCS_CHAN_WIDTH_20;

		WARN_ON_ONCE(rate->index < IWL_RATE_MCS_0_INDEX ||
			     rate->index > IWL_RATE_MCS_9_INDEX);

		rate->index = rs_ht_to_legacy[rate->index];
		rate->ldpc = false;
	} else {
		/* Downgrade to SISO with same MCS if in MIMO  */
		rate->type = is_vht_mimo2(rate) ?
			LQ_VHT_SISO : LQ_HT_SISO;
	}

	if (num_of_ant(rate->ant) > 1)
		rate->ant = first_antenna(mvm->fw->valid_tx_ant);

	/* Relevant in both switching to SISO or Legacy */
	rate->sgi = false;

	if (!rs_rate_supported(lq_sta, rate))
		rs_get_lower_rate_in_column(lq_sta, rate);
}

/* Simple function to compare two rate scale table types */
static inline bool rs_rate_match(struct rs_rate *a,
				 struct rs_rate *b)
{
	return (a->type == b->type) && (a->ant == b->ant) && (a->sgi == b->sgi);
}

static u32 rs_ch_width_from_mac_flags(enum mac80211_rate_control_flags flags)
{
	if (flags & IEEE80211_TX_RC_40_MHZ_WIDTH)
		return RATE_MCS_CHAN_WIDTH_40;
	else if (flags & IEEE80211_TX_RC_80_MHZ_WIDTH)
		return RATE_MCS_CHAN_WIDTH_80;
	else if (flags & IEEE80211_TX_RC_160_MHZ_WIDTH)
		return RATE_MCS_CHAN_WIDTH_160;

	return RATE_MCS_CHAN_WIDTH_20;
}

/*
 * mac80211 sends us Tx status
 */
static void rs_tx_status(void *mvm_r, struct ieee80211_supported_band *sband,
			 struct ieee80211_sta *sta, void *priv_sta,
			 struct sk_buff *skb)
{
	int legacy_success;
	int retries;
	int mac_index, i;
	struct iwl_lq_sta *lq_sta = priv_sta;
	struct iwl_lq_cmd *table;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct iwl_op_mode *op_mode = (struct iwl_op_mode *)mvm_r;
	struct iwl_mvm *mvm = IWL_OP_MODE_GET_MVM(op_mode);
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	enum mac80211_rate_control_flags mac_flags;
	u32 ucode_rate;
	struct rs_rate rate;
	struct iwl_scale_tbl_info *curr_tbl, *other_tbl, *tmp_tbl;
	u8 reduced_txp = (uintptr_t)info->status.status_driver_data[0];

	/* Treat uninitialized rate scaling data same as non-existing. */
	if (!lq_sta) {
		IWL_DEBUG_RATE(mvm, "Station rate scaling not created yet.\n");
		return;
	} else if (!lq_sta->pers.drv) {
		IWL_DEBUG_RATE(mvm, "Rate scaling not initialized yet.\n");
		return;
	}

#ifdef CPTCFG_MAC80211_DEBUGFS
	/* Disable last tx check if we are debugging with fixed rate */
	if (lq_sta->pers.dbg_fixed_rate) {
		IWL_DEBUG_RATE(mvm, "Fixed rate. avoid rate scaling\n");
		return;
	}
#endif
	if (!ieee80211_is_data(hdr->frame_control) ||
	    info->flags & IEEE80211_TX_CTL_NO_ACK)
		return;

	/* This packet was aggregated but doesn't carry status info */
	if ((info->flags & IEEE80211_TX_CTL_AMPDU) &&
	    !(info->flags & IEEE80211_TX_STAT_AMPDU))
		return;

	/*
	 * Ignore this Tx frame response if its initial rate doesn't match
	 * that of latest Link Quality command.  There may be stragglers
	 * from a previous Link Quality command, but we're no longer interested
	 * in those; they're either from the "active" mode while we're trying
	 * to check "search" mode, or a prior "search" mode after we've moved
	 * to a new "search" mode (which might become the new "active" mode).
	 */
	table = &lq_sta->lq;
	ucode_rate = le32_to_cpu(table->rs_table[0]);
	rs_rate_from_ucode_rate(ucode_rate, info->band, &rate);
	if (info->band == IEEE80211_BAND_5GHZ)
		rate.index -= IWL_FIRST_OFDM_RATE;
	mac_flags = info->status.rates[0].flags;
	mac_index = info->status.rates[0].idx;
	/* For HT packets, map MCS to PLCP */
	if (mac_flags & IEEE80211_TX_RC_MCS) {
		/* Remove # of streams */
		mac_index &= RATE_HT_MCS_RATE_CODE_MSK;
		if (mac_index >= (IWL_RATE_9M_INDEX - IWL_FIRST_OFDM_RATE))
			mac_index++;
		/*
		 * mac80211 HT index is always zero-indexed; we need to move
		 * HT OFDM rates after CCK rates in 2.4 GHz band
		 */
		if (info->band == IEEE80211_BAND_2GHZ)
			mac_index += IWL_FIRST_OFDM_RATE;
	} else if (mac_flags & IEEE80211_TX_RC_VHT_MCS) {
		mac_index &= RATE_VHT_MCS_RATE_CODE_MSK;
		if (mac_index >= (IWL_RATE_9M_INDEX - IWL_FIRST_OFDM_RATE))
			mac_index++;
	}

	if (time_after(jiffies,
		       (unsigned long)(lq_sta->last_tx + RS_IDLE_TIMEOUT))) {
		int tid;
		IWL_DEBUG_RATE(mvm, "Tx idle for too long. reinit rs\n");
		for (tid = 0; tid < IWL_MAX_TID_COUNT; tid++)
			ieee80211_stop_tx_ba_session(sta, tid);

		iwl_mvm_rs_rate_init(mvm, sta, sband->band, false);
		return;
	}
	lq_sta->last_tx = jiffies;

	/* Here we actually compare this rate to the latest LQ command */
	if ((mac_index < 0) ||
	    (rate.sgi != !!(mac_flags & IEEE80211_TX_RC_SHORT_GI)) ||
	    (rate.bw != rs_ch_width_from_mac_flags(mac_flags)) ||
	    (rate.ant != info->status.antenna) ||
	    (!!(ucode_rate & RATE_MCS_HT_MSK) !=
	     !!(mac_flags & IEEE80211_TX_RC_MCS)) ||
	    (!!(ucode_rate & RATE_MCS_VHT_MSK) !=
	     !!(mac_flags & IEEE80211_TX_RC_VHT_MCS)) ||
	    (!!(ucode_rate & RATE_HT_MCS_GF_MSK) !=
	     !!(mac_flags & IEEE80211_TX_RC_GREEN_FIELD)) ||
	    (rate.index != mac_index)) {
		IWL_DEBUG_RATE(mvm,
			       "initial rate %d does not match %d (0x%x)\n",
			       mac_index, rate.index, ucode_rate);
		/*
		 * Since rates mis-match, the last LQ command may have failed.
		 * After IWL_MISSED_RATE_MAX mis-matches, resync the uCode with
		 * ... driver.
		 */
		lq_sta->missed_rate_counter++;
		if (lq_sta->missed_rate_counter > IWL_MISSED_RATE_MAX) {
			lq_sta->missed_rate_counter = 0;
			IWL_DEBUG_RATE(mvm,
				       "Too many rates mismatch. Send sync LQ. rs_state %d\n",
				       lq_sta->rs_state);
			iwl_mvm_send_lq_cmd(mvm, &lq_sta->lq, false);
		}
		/* Regardless, ignore this status info for outdated rate */
		return;
	} else
		/* Rate did match, so reset the missed_rate_counter */
		lq_sta->missed_rate_counter = 0;

	/* Figure out if rate scale algorithm is in active or search table */
	if (rs_rate_match(&rate,
			  &(lq_sta->lq_info[lq_sta->active_tbl].rate))) {
		curr_tbl = &(lq_sta->lq_info[lq_sta->active_tbl]);
		other_tbl = &(lq_sta->lq_info[1 - lq_sta->active_tbl]);
	} else if (rs_rate_match(&rate,
			 &lq_sta->lq_info[1 - lq_sta->active_tbl].rate)) {
		curr_tbl = &(lq_sta->lq_info[1 - lq_sta->active_tbl]);
		other_tbl = &(lq_sta->lq_info[lq_sta->active_tbl]);
	} else {
		IWL_DEBUG_RATE(mvm,
			       "Neither active nor search matches tx rate\n");
		tmp_tbl = &(lq_sta->lq_info[lq_sta->active_tbl]);
		rs_dump_rate(mvm, &tmp_tbl->rate, "ACTIVE");
		tmp_tbl = &(lq_sta->lq_info[1 - lq_sta->active_tbl]);
		rs_dump_rate(mvm, &tmp_tbl->rate, "SEARCH");
		rs_dump_rate(mvm, &rate, "ACTUAL");

		/*
		 * no matching table found, let's by-pass the data collection
		 * and continue to perform rate scale to find the rate table
		 */
		rs_stay_in_table(lq_sta, true);
		goto done;
	}

	/*
	 * Updating the frame history depends on whether packets were
	 * aggregated.
	 *
	 * For aggregation, all packets were transmitted at the same rate, the
	 * first index into rate scale table.
	 */
	if (info->flags & IEEE80211_TX_STAT_AMPDU) {
		ucode_rate = le32_to_cpu(table->rs_table[0]);
		rs_rate_from_ucode_rate(ucode_rate, info->band, &rate);
		rs_collect_tx_data(lq_sta, curr_tbl, rate.index,
				   info->status.ampdu_len,
				   info->status.ampdu_ack_len,
				   reduced_txp);

		/* Update success/fail counts if not searching for new mode */
		if (lq_sta->rs_state == RS_STATE_STAY_IN_COLUMN) {
			lq_sta->total_success += info->status.ampdu_ack_len;
			lq_sta->total_failed += (info->status.ampdu_len -
					info->status.ampdu_ack_len);
		}
	} else {
	/*
	 * For legacy, update frame history with for each Tx retry.
	 */
		retries = info->status.rates[0].count - 1;
		/* HW doesn't send more than 15 retries */
		retries = min(retries, 15);

		/* The last transmission may have been successful */
		legacy_success = !!(info->flags & IEEE80211_TX_STAT_ACK);
		/* Collect data for each rate used during failed TX attempts */
		for (i = 0; i <= retries; ++i) {
			ucode_rate = le32_to_cpu(table->rs_table[i]);
			rs_rate_from_ucode_rate(ucode_rate, info->band, &rate);
			/*
			 * Only collect stats if retried rate is in the same RS
			 * table as active/search.
			 */
			if (rs_rate_match(&rate, &curr_tbl->rate))
				tmp_tbl = curr_tbl;
			else if (rs_rate_match(&rate, &other_tbl->rate))
				tmp_tbl = other_tbl;
			else
				continue;

			rs_collect_tx_data(lq_sta, tmp_tbl, rate.index, 1,
					   i < retries ? 0 : legacy_success,
					   reduced_txp);
		}

		/* Update success/fail counts if not searching for new mode */
		if (lq_sta->rs_state == RS_STATE_STAY_IN_COLUMN) {
			lq_sta->total_success += legacy_success;
			lq_sta->total_failed += retries + (1 - legacy_success);
		}
	}
	/* The last TX rate is cached in lq_sta; it's set in if/else above */
	lq_sta->last_rate_n_flags = ucode_rate;
	IWL_DEBUG_RATE(mvm, "reduced txpower: %d\n", reduced_txp);
done:
	/* See if there's a better rate or modulation mode to try. */
	if (sta && sta->supp_rates[sband->band])
		rs_rate_scale_perform(mvm, skb, sta, lq_sta);
}

/*
 * Begin a period of staying with a selected modulation mode.
 * Set "stay_in_tbl" flag to prevent any mode switches.
 * Set frame tx success limits according to legacy vs. high-throughput,
 * and reset overall (spanning all rates) tx success history statistics.
 * These control how long we stay using same modulation mode before
 * searching for a new mode.
 */
static void rs_set_stay_in_table(struct iwl_mvm *mvm, u8 is_legacy,
				 struct iwl_lq_sta *lq_sta)
{
	IWL_DEBUG_RATE(mvm, "Moving to RS_STATE_STAY_IN_COLUMN\n");
	lq_sta->rs_state = RS_STATE_STAY_IN_COLUMN;
	if (is_legacy) {
		lq_sta->table_count_limit = IWL_LEGACY_TABLE_COUNT;
		lq_sta->max_failure_limit = IWL_LEGACY_FAILURE_LIMIT;
		lq_sta->max_success_limit = IWL_LEGACY_SUCCESS_LIMIT;
	} else {
		lq_sta->table_count_limit = IWL_NONE_LEGACY_TABLE_COUNT;
		lq_sta->max_failure_limit = IWL_NONE_LEGACY_FAILURE_LIMIT;
		lq_sta->max_success_limit = IWL_NONE_LEGACY_SUCCESS_LIMIT;
	}
	lq_sta->table_count = 0;
	lq_sta->total_failed = 0;
	lq_sta->total_success = 0;
	lq_sta->flush_timer = jiffies;
	lq_sta->visited_columns = 0;
}

static int rs_get_max_allowed_rate(struct iwl_lq_sta *lq_sta,
				   const struct rs_tx_column *column)
{
	switch (column->mode) {
	case RS_LEGACY:
		return lq_sta->max_legacy_rate_idx;
	case RS_SISO:
		return lq_sta->max_siso_rate_idx;
	case RS_MIMO2:
		return lq_sta->max_mimo2_rate_idx;
	default:
		WARN_ON_ONCE(1);
	}

	return lq_sta->max_legacy_rate_idx;
}

static const u16 *rs_get_expected_tpt_table(struct iwl_lq_sta *lq_sta,
					    const struct rs_tx_column *column,
					    u32 bw)
{
	/* Used to choose among HT tables */
	const u16 (*ht_tbl_pointer)[IWL_RATE_COUNT];

	if (WARN_ON_ONCE(column->mode != RS_LEGACY &&
			 column->mode != RS_SISO &&
			 column->mode != RS_MIMO2))
		return expected_tpt_legacy;

	/* Legacy rates have only one table */
	if (column->mode == RS_LEGACY)
		return expected_tpt_legacy;

	ht_tbl_pointer = expected_tpt_mimo2_20MHz;
	/* Choose among many HT tables depending on number of streams
	 * (SISO/MIMO2), channel width (20/40/80), SGI, and aggregation
	 * status */
	if (column->mode == RS_SISO) {
		switch (bw) {
		case RATE_MCS_CHAN_WIDTH_20:
			ht_tbl_pointer = expected_tpt_siso_20MHz;
			break;
		case RATE_MCS_CHAN_WIDTH_40:
			ht_tbl_pointer = expected_tpt_siso_40MHz;
			break;
		case RATE_MCS_CHAN_WIDTH_80:
			ht_tbl_pointer = expected_tpt_siso_80MHz;
			break;
		default:
			WARN_ON_ONCE(1);
		}
	} else if (column->mode == RS_MIMO2) {
		switch (bw) {
		case RATE_MCS_CHAN_WIDTH_20:
			ht_tbl_pointer = expected_tpt_mimo2_20MHz;
			break;
		case RATE_MCS_CHAN_WIDTH_40:
			ht_tbl_pointer = expected_tpt_mimo2_40MHz;
			break;
		case RATE_MCS_CHAN_WIDTH_80:
			ht_tbl_pointer = expected_tpt_mimo2_80MHz;
			break;
		default:
			WARN_ON_ONCE(1);
		}
	} else {
		WARN_ON_ONCE(1);
	}

	if (!column->sgi && !lq_sta->is_agg)		/* Normal */
		return ht_tbl_pointer[0];
	else if (column->sgi && !lq_sta->is_agg)        /* SGI */
		return ht_tbl_pointer[1];
	else if (!column->sgi && lq_sta->is_agg)        /* AGG */
		return ht_tbl_pointer[2];
	else						/* AGG+SGI */
		return ht_tbl_pointer[3];
}

static void rs_set_expected_tpt_table(struct iwl_lq_sta *lq_sta,
				      struct iwl_scale_tbl_info *tbl)
{
	struct rs_rate *rate = &tbl->rate;
	const struct rs_tx_column *column = &rs_tx_columns[tbl->column];

	tbl->expected_tpt = rs_get_expected_tpt_table(lq_sta, column, rate->bw);
}

static s32 rs_get_best_rate(struct iwl_mvm *mvm,
			    struct iwl_lq_sta *lq_sta,
			    struct iwl_scale_tbl_info *tbl,	/* "search" */
			    unsigned long rate_mask, s8 index)
{
	struct iwl_scale_tbl_info *active_tbl =
	    &(lq_sta->lq_info[lq_sta->active_tbl]);
	s32 success_ratio = active_tbl->win[index].success_ratio;
	u16 expected_current_tpt = active_tbl->expected_tpt[index];
	const u16 *tpt_tbl = tbl->expected_tpt;
	u16 high_low;
	u32 target_tpt;
	int rate_idx;

	if (success_ratio > RS_SR_NO_DECREASE) {
		target_tpt = 100 * expected_current_tpt;
		IWL_DEBUG_RATE(mvm,
			       "SR %d high. Find rate exceeding EXPECTED_CURRENT %d\n",
			       success_ratio, target_tpt);
	} else {
		target_tpt = lq_sta->last_tpt;
		IWL_DEBUG_RATE(mvm,
			       "SR %d not thag good. Find rate exceeding ACTUAL_TPT %d\n",
			       success_ratio, target_tpt);
	}

	rate_idx = find_first_bit(&rate_mask, BITS_PER_LONG);

	while (rate_idx != IWL_RATE_INVALID) {
		if (target_tpt < (100 * tpt_tbl[rate_idx]))
			break;

		high_low = rs_get_adjacent_rate(mvm, rate_idx, rate_mask,
						tbl->rate.type);

		rate_idx = (high_low >> 8) & 0xff;
	}

	IWL_DEBUG_RATE(mvm, "Best rate found %d target_tp %d expected_new %d\n",
		       rate_idx, target_tpt,
		       rate_idx != IWL_RATE_INVALID ?
		       100 * tpt_tbl[rate_idx] : IWL_INVALID_VALUE);

	return rate_idx;
}

static u32 rs_bw_from_sta_bw(struct ieee80211_sta *sta)
{
	if (sta->bandwidth >= IEEE80211_STA_RX_BW_80)
		return RATE_MCS_CHAN_WIDTH_80;
	else if (sta->bandwidth >= IEEE80211_STA_RX_BW_40)
		return RATE_MCS_CHAN_WIDTH_40;

	return RATE_MCS_CHAN_WIDTH_20;
}

/*
 * Check whether we should continue using same modulation mode, or
 * begin search for a new mode, based on:
 * 1) # tx successes or failures while using this mode
 * 2) # times calling this function
 * 3) elapsed time in this mode (not used, for now)
 */
static void rs_stay_in_table(struct iwl_lq_sta *lq_sta, bool force_search)
{
	struct iwl_scale_tbl_info *tbl;
	int active_tbl;
	int flush_interval_passed = 0;
	struct iwl_mvm *mvm;

	mvm = lq_sta->pers.drv;
	active_tbl = lq_sta->active_tbl;

	tbl = &(lq_sta->lq_info[active_tbl]);

	/* If we've been disallowing search, see if we should now allow it */
	if (lq_sta->rs_state == RS_STATE_STAY_IN_COLUMN) {
		/* Elapsed time using current modulation mode */
		if (lq_sta->flush_timer)
			flush_interval_passed =
				time_after(jiffies,
					   (unsigned long)(lq_sta->flush_timer +
						RS_STAY_IN_COLUMN_TIMEOUT));

		/*
		 * Check if we should allow search for new modulation mode.
		 * If many frames have failed or succeeded, or we've used
		 * this same modulation for a long time, allow search, and
		 * reset history stats that keep track of whether we should
		 * allow a new search.  Also (below) reset all bitmaps and
		 * stats in active history.
		 */
		if (force_search ||
		    (lq_sta->total_failed > lq_sta->max_failure_limit) ||
		    (lq_sta->total_success > lq_sta->max_success_limit) ||
		    ((!lq_sta->search_better_tbl) &&
		     (lq_sta->flush_timer) && (flush_interval_passed))) {
			IWL_DEBUG_RATE(mvm,
				       "LQ: stay is expired %d %d %d\n",
				     lq_sta->total_failed,
				     lq_sta->total_success,
				     flush_interval_passed);

			/* Allow search for new mode */
			lq_sta->rs_state = RS_STATE_SEARCH_CYCLE_STARTED;
			IWL_DEBUG_RATE(mvm,
				       "Moving to RS_STATE_SEARCH_CYCLE_STARTED\n");
			lq_sta->total_failed = 0;
			lq_sta->total_success = 0;
			lq_sta->flush_timer = 0;
			/* mark the current column as visited */
			lq_sta->visited_columns = BIT(tbl->column);
		/*
		 * Else if we've used this modulation mode enough repetitions
		 * (regardless of elapsed time or success/failure), reset
		 * history bitmaps and rate-specific stats for all rates in
		 * active table.
		 */
		} else {
			lq_sta->table_count++;
			if (lq_sta->table_count >=
			    lq_sta->table_count_limit) {
				lq_sta->table_count = 0;

				IWL_DEBUG_RATE(mvm,
					       "LQ: stay in table clear win\n");
				rs_rate_scale_clear_tbl_windows(mvm, tbl);
			}
		}

		/* If transitioning to allow "search", reset all history
		 * bitmaps and stats in active table (this will become the new
		 * "search" table). */
		if (lq_sta->rs_state == RS_STATE_SEARCH_CYCLE_STARTED) {
			rs_rate_scale_clear_tbl_windows(mvm, tbl);
		}
	}
}

/*
 * setup rate table in uCode
 */
static void rs_update_rate_tbl(struct iwl_mvm *mvm,
			       struct ieee80211_sta *sta,
			       struct iwl_lq_sta *lq_sta,
			       struct rs_rate *rate)
{
	rs_fill_lq_cmd(mvm, sta, lq_sta, rate);
	iwl_mvm_send_lq_cmd(mvm, &lq_sta->lq, false);
}

static u8 rs_get_tid(struct iwl_lq_sta *lq_data,
		     struct ieee80211_hdr *hdr)
{
	u8 tid = IWL_MAX_TID_COUNT;

	if (ieee80211_is_data_qos(hdr->frame_control)) {
		u8 *qc = ieee80211_get_qos_ctl(hdr);
		tid = qc[0] & 0xf;
	}

	if (unlikely(tid > IWL_MAX_TID_COUNT))
		tid = IWL_MAX_TID_COUNT;

	return tid;
}

static enum rs_column rs_get_next_column(struct iwl_mvm *mvm,
					 struct iwl_lq_sta *lq_sta,
					 struct ieee80211_sta *sta,
					 struct iwl_scale_tbl_info *tbl)
{
	int i, j, max_rate;
	enum rs_column next_col_id;
	const struct rs_tx_column *curr_col = &rs_tx_columns[tbl->column];
	const struct rs_tx_column *next_col;
	allow_column_func_t allow_func;
	u8 valid_ants = mvm->fw->valid_tx_ant;
	const u16 *expected_tpt_tbl;
	u16 tpt, max_expected_tpt;

	for (i = 0; i < MAX_NEXT_COLUMNS; i++) {
		next_col_id = curr_col->next_columns[i];

		if (next_col_id == RS_COLUMN_INVALID)
			continue;

		if (lq_sta->visited_columns & BIT(next_col_id)) {
			IWL_DEBUG_RATE(mvm, "Skip already visited column %d\n",
				       next_col_id);
			continue;
		}

		next_col = &rs_tx_columns[next_col_id];

		if (!rs_is_valid_ant(valid_ants, next_col->ant)) {
			IWL_DEBUG_RATE(mvm,
				       "Skip column %d as ANT config isn't supported by chip. valid_ants 0x%x column ant 0x%x\n",
				       next_col_id, valid_ants, next_col->ant);
			continue;
		}

		for (j = 0; j < MAX_COLUMN_CHECKS; j++) {
			allow_func = next_col->checks[j];
			if (allow_func && !allow_func(mvm, sta, tbl))
				break;
		}

		if (j != MAX_COLUMN_CHECKS) {
			IWL_DEBUG_RATE(mvm,
				       "Skip column %d: not allowed (check %d failed)\n",
				       next_col_id, j);

			continue;
		}

		tpt = lq_sta->last_tpt / 100;
		expected_tpt_tbl = rs_get_expected_tpt_table(lq_sta, next_col,
						     rs_bw_from_sta_bw(sta));
		if (WARN_ON_ONCE(!expected_tpt_tbl))
			continue;

		max_rate = rs_get_max_allowed_rate(lq_sta, next_col);
		if (WARN_ON_ONCE(max_rate == IWL_RATE_INVALID))
			continue;

		max_expected_tpt = expected_tpt_tbl[max_rate];
		if (tpt >= max_expected_tpt) {
			IWL_DEBUG_RATE(mvm,
				       "Skip column %d: can't beat current TPT. Max expected %d current %d\n",
				       next_col_id, max_expected_tpt, tpt);
			continue;
		}

		IWL_DEBUG_RATE(mvm,
			       "Found potential column %d. Max expected %d current %d\n",
			       next_col_id, max_expected_tpt, tpt);
		break;
	}

	if (i == MAX_NEXT_COLUMNS)
		return RS_COLUMN_INVALID;

	return next_col_id;
}

static int rs_switch_to_column(struct iwl_mvm *mvm,
			       struct iwl_lq_sta *lq_sta,
			       struct ieee80211_sta *sta,
			       enum rs_column col_id)
{
	struct iwl_scale_tbl_info *tbl = &(lq_sta->lq_info[lq_sta->active_tbl]);
	struct iwl_scale_tbl_info *search_tbl =
				&(lq_sta->lq_info[(1 - lq_sta->active_tbl)]);
	struct rs_rate *rate = &search_tbl->rate;
	const struct rs_tx_column *column = &rs_tx_columns[col_id];
	const struct rs_tx_column *curr_column = &rs_tx_columns[tbl->column];
	u32 sz = (sizeof(struct iwl_scale_tbl_info) -
		  (sizeof(struct iwl_rate_scale_data) * IWL_RATE_COUNT));
	unsigned long rate_mask = 0;
	u32 rate_idx = 0;

	memcpy(search_tbl, tbl, sz);

	rate->sgi = column->sgi;
	rate->ant = column->ant;

	if (column->mode == RS_LEGACY) {
		if (lq_sta->band == IEEE80211_BAND_5GHZ)
			rate->type = LQ_LEGACY_A;
		else
			rate->type = LQ_LEGACY_G;

		rate_mask = lq_sta->active_legacy_rate;
	} else if (column->mode == RS_SISO) {
		rate->type = lq_sta->is_vht ? LQ_VHT_SISO : LQ_HT_SISO;
		rate_mask = lq_sta->active_siso_rate;
	} else if (column->mode == RS_MIMO2) {
		rate->type = lq_sta->is_vht ? LQ_VHT_MIMO2 : LQ_HT_MIMO2;
		rate_mask = lq_sta->active_mimo2_rate;
	} else {
		WARN_ON_ONCE("Bad column mode");
	}

	rate->bw = rs_bw_from_sta_bw(sta);
	rate->ldpc = lq_sta->ldpc;
	search_tbl->column = col_id;
	rs_set_expected_tpt_table(lq_sta, search_tbl);

	lq_sta->visited_columns |= BIT(col_id);

	/* Get the best matching rate if we're changing modes. e.g.
	 * SISO->MIMO, LEGACY->SISO, MIMO->SISO
	 */
	if (curr_column->mode != column->mode) {
		rate_idx = rs_get_best_rate(mvm, lq_sta, search_tbl,
					    rate_mask, rate->index);

		if ((rate_idx == IWL_RATE_INVALID) ||
		    !(BIT(rate_idx) & rate_mask)) {
			IWL_DEBUG_RATE(mvm,
				       "can not switch with index %d"
				       " rate mask %lx\n",
				       rate_idx, rate_mask);

			goto err;
		}

		rate->index = rate_idx;
	}

	IWL_DEBUG_RATE(mvm, "Switched to column %d: Index %d\n",
		       col_id, rate->index);

	return 0;

err:
	rate->type = LQ_NONE;
	return -1;
}

static enum rs_action rs_get_rate_action(struct iwl_mvm *mvm,
					 struct iwl_scale_tbl_info *tbl,
					 s32 sr, int low, int high,
					 int current_tpt,
					 int low_tpt, int high_tpt)
{
	enum rs_action action = RS_ACTION_STAY;

	if ((sr <= RS_SR_FORCE_DECREASE) || (current_tpt == 0)) {
		IWL_DEBUG_RATE(mvm,
			       "Decrease rate because of low SR\n");
		return RS_ACTION_DOWNSCALE;
	}

	if ((low_tpt == IWL_INVALID_VALUE) &&
	    (high_tpt == IWL_INVALID_VALUE) &&
	    (high != IWL_RATE_INVALID)) {
		IWL_DEBUG_RATE(mvm,
			       "No data about high/low rates. Increase rate\n");
		return RS_ACTION_UPSCALE;
	}

	if ((high_tpt == IWL_INVALID_VALUE) &&
	    (high != IWL_RATE_INVALID) &&
	    (low_tpt != IWL_INVALID_VALUE) &&
	    (low_tpt < current_tpt)) {
		IWL_DEBUG_RATE(mvm,
			       "No data about high rate and low rate is worse. Increase rate\n");
		return RS_ACTION_UPSCALE;
	}

	if ((high_tpt != IWL_INVALID_VALUE) &&
	    (high_tpt > current_tpt)) {
		IWL_DEBUG_RATE(mvm,
			       "Higher rate is better. Increate rate\n");
		return RS_ACTION_UPSCALE;
	}

	if ((low_tpt != IWL_INVALID_VALUE) &&
	    (high_tpt != IWL_INVALID_VALUE) &&
	    (low_tpt < current_tpt) &&
	    (high_tpt < current_tpt)) {
		IWL_DEBUG_RATE(mvm,
			       "Both high and low are worse. Maintain rate\n");
		return RS_ACTION_STAY;
	}

	if ((low_tpt != IWL_INVALID_VALUE) &&
	    (low_tpt > current_tpt)) {
		IWL_DEBUG_RATE(mvm,
			       "Lower rate is better\n");
		action = RS_ACTION_DOWNSCALE;
		goto out;
	}

	if ((low_tpt == IWL_INVALID_VALUE) &&
	    (low != IWL_RATE_INVALID)) {
		IWL_DEBUG_RATE(mvm,
			       "No data about lower rate\n");
		action = RS_ACTION_DOWNSCALE;
		goto out;
	}

	IWL_DEBUG_RATE(mvm, "Maintain rate\n");

out:
	if ((action == RS_ACTION_DOWNSCALE) && (low != IWL_RATE_INVALID)) {
		if (sr >= RS_SR_NO_DECREASE) {
			IWL_DEBUG_RATE(mvm,
				       "SR is above NO DECREASE. Avoid downscale\n");
			action = RS_ACTION_STAY;
		} else if (current_tpt > (100 * tbl->expected_tpt[low])) {
			IWL_DEBUG_RATE(mvm,
				       "Current TPT is higher than max expected in low rate. Avoid downscale\n");
			action = RS_ACTION_STAY;
		} else {
			IWL_DEBUG_RATE(mvm, "Decrease rate\n");
		}
	}

	return action;
}

static void rs_get_adjacent_txp(struct iwl_mvm *mvm, int index,
				int *weaker, int *stronger)
{
	*weaker = index + TPC_TX_POWER_STEP;
	if (*weaker > TPC_MAX_REDUCTION)
		*weaker = TPC_INVALID;

	*stronger = index - TPC_TX_POWER_STEP;
	if (*stronger < 0)
		*stronger = TPC_INVALID;
}

static bool rs_tpc_allowed(struct iwl_mvm *mvm, struct ieee80211_vif *vif,
			   struct rs_rate *rate, enum ieee80211_band band)
{
	int index = rate->index;
	bool cam = (iwlmvm_mod_params.power_scheme == IWL_POWER_SCHEME_CAM);
	bool sta_ps_disabled = (vif->type == NL80211_IFTYPE_STATION &&
				!vif->bss_conf.ps);

	IWL_DEBUG_RATE(mvm, "cam: %d sta_ps_disabled %d\n",
		       cam, sta_ps_disabled);
	/*
	 * allow tpc only if power management is enabled, or bt coex
	 * activity grade allows it and we are on 2.4Ghz.
	 */
	if ((cam || sta_ps_disabled) &&
	    !iwl_mvm_bt_coex_is_tpc_allowed(mvm, band))
		return false;

	IWL_DEBUG_RATE(mvm, "check rate, table type: %d\n", rate->type);
	if (is_legacy(rate))
		return index == IWL_RATE_54M_INDEX;
	if (is_ht(rate))
		return index == IWL_RATE_MCS_7_INDEX;
	if (is_vht(rate))
		return index == IWL_RATE_MCS_7_INDEX ||
		       index == IWL_RATE_MCS_8_INDEX ||
		       index == IWL_RATE_MCS_9_INDEX;

	WARN_ON_ONCE(1);
	return false;
}

enum tpc_action {
	TPC_ACTION_STAY,
	TPC_ACTION_DECREASE,
	TPC_ACTION_INCREASE,
	TPC_ACTION_NO_RESTIRCTION,
};

static enum tpc_action rs_get_tpc_action(struct iwl_mvm *mvm,
					 s32 sr, int weak, int strong,
					 int current_tpt,
					 int weak_tpt, int strong_tpt)
{
	/* stay until we have valid tpt */
	if (current_tpt == IWL_INVALID_VALUE) {
		IWL_DEBUG_RATE(mvm, "no current tpt. stay.\n");
		return TPC_ACTION_STAY;
	}

	/* Too many failures, increase txp */
	if (sr <= TPC_SR_FORCE_INCREASE || current_tpt == 0) {
		IWL_DEBUG_RATE(mvm, "increase txp because of weak SR\n");
		return TPC_ACTION_NO_RESTIRCTION;
	}

	/* try decreasing first if applicable */
	if (weak != TPC_INVALID) {
		if (weak_tpt == IWL_INVALID_VALUE &&
		    (strong_tpt == IWL_INVALID_VALUE ||
		     current_tpt >= strong_tpt)) {
			IWL_DEBUG_RATE(mvm,
				       "no weak txp measurement. decrease txp\n");
			return TPC_ACTION_DECREASE;
		}

		if (weak_tpt > current_tpt) {
			IWL_DEBUG_RATE(mvm,
				       "lower txp has better tpt. decrease txp\n");
			return TPC_ACTION_DECREASE;
		}
	}

	/* next, increase if needed */
	if (sr < TPC_SR_NO_INCREASE && strong != TPC_INVALID) {
		if (weak_tpt == IWL_INVALID_VALUE &&
		    strong_tpt != IWL_INVALID_VALUE &&
		    current_tpt < strong_tpt) {
			IWL_DEBUG_RATE(mvm,
				       "higher txp has better tpt. increase txp\n");
			return TPC_ACTION_INCREASE;
		}

		if (weak_tpt < current_tpt &&
		    (strong_tpt == IWL_INVALID_VALUE ||
		     strong_tpt > current_tpt)) {
			IWL_DEBUG_RATE(mvm,
				       "lower txp has worse tpt. increase txp\n");
			return TPC_ACTION_INCREASE;
		}
	}

	IWL_DEBUG_RATE(mvm, "no need to increase or decrease txp - stay\n");
	return TPC_ACTION_STAY;
}

static bool rs_tpc_perform(struct iwl_mvm *mvm,
			   struct ieee80211_sta *sta,
			   struct iwl_lq_sta *lq_sta,
			   struct iwl_scale_tbl_info *tbl)
{
	struct iwl_mvm_sta *mvm_sta = (void *)sta->drv_priv;
	struct ieee80211_vif *vif = mvm_sta->vif;
	struct ieee80211_chanctx_conf *chanctx_conf;
	enum ieee80211_band band;
	struct iwl_rate_scale_data *window;
	struct rs_rate *rate = &tbl->rate;
	enum tpc_action action;
	s32 sr;
	u8 cur = lq_sta->lq.reduced_tpc;
	int current_tpt;
	int weak, strong;
	int weak_tpt = IWL_INVALID_VALUE, strong_tpt = IWL_INVALID_VALUE;

#ifdef CPTCFG_MAC80211_DEBUGFS
	if (lq_sta->pers.dbg_fixed_txp_reduction <= TPC_MAX_REDUCTION) {
		IWL_DEBUG_RATE(mvm, "fixed tpc: %d",
			       lq_sta->pers.dbg_fixed_txp_reduction);
		lq_sta->lq.reduced_tpc = lq_sta->pers.dbg_fixed_txp_reduction;
		return cur != lq_sta->pers.dbg_fixed_txp_reduction;
	}
#endif

	rcu_read_lock();
	chanctx_conf = rcu_dereference(vif->chanctx_conf);
	if (WARN_ON(!chanctx_conf))
		band = IEEE80211_NUM_BANDS;
	else
		band = chanctx_conf->def.chan->band;
	rcu_read_unlock();

	if (!rs_tpc_allowed(mvm, vif, rate, band)) {
		IWL_DEBUG_RATE(mvm,
			       "tpc is not allowed. remove txp restrictions");
		lq_sta->lq.reduced_tpc = TPC_NO_REDUCTION;
		return cur != TPC_NO_REDUCTION;
	}

	rs_get_adjacent_txp(mvm, cur, &weak, &strong);

	/* Collect measured throughputs for current and adjacent rates */
	window = tbl->tpc_win;
	sr = window[cur].success_ratio;
	current_tpt = window[cur].average_tpt;
	if (weak != TPC_INVALID)
		weak_tpt = window[weak].average_tpt;
	if (strong != TPC_INVALID)
		strong_tpt = window[strong].average_tpt;

	IWL_DEBUG_RATE(mvm,
		       "(TPC: %d): cur_tpt %d SR %d weak %d strong %d weak_tpt %d strong_tpt %d\n",
		       cur, current_tpt, sr, weak, strong,
		       weak_tpt, strong_tpt);

	action = rs_get_tpc_action(mvm, sr, weak, strong,
				   current_tpt, weak_tpt, strong_tpt);

	/* override actions if we are on the edge */
	if (weak == TPC_INVALID && action == TPC_ACTION_DECREASE) {
		IWL_DEBUG_RATE(mvm, "already in lowest txp, stay");
		action = TPC_ACTION_STAY;
	} else if (strong == TPC_INVALID &&
		   (action == TPC_ACTION_INCREASE ||
		    action == TPC_ACTION_NO_RESTIRCTION)) {
		IWL_DEBUG_RATE(mvm, "already in highest txp, stay");
		action = TPC_ACTION_STAY;
	}

	switch (action) {
	case TPC_ACTION_DECREASE:
		lq_sta->lq.reduced_tpc = weak;
		return true;
	case TPC_ACTION_INCREASE:
		lq_sta->lq.reduced_tpc = strong;
		return true;
	case TPC_ACTION_NO_RESTIRCTION:
		lq_sta->lq.reduced_tpc = TPC_NO_REDUCTION;
		return true;
	case TPC_ACTION_STAY:
		/* do nothing */
		break;
	}
	return false;
}

/*
 * Do rate scaling and search for new modulation mode.
 */
static void rs_rate_scale_perform(struct iwl_mvm *mvm,
				  struct sk_buff *skb,
				  struct ieee80211_sta *sta,
				  struct iwl_lq_sta *lq_sta)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	int low = IWL_RATE_INVALID;
	int high = IWL_RATE_INVALID;
	int index;
	struct iwl_rate_scale_data *window = NULL;
	int current_tpt = IWL_INVALID_VALUE;
	int low_tpt = IWL_INVALID_VALUE;
	int high_tpt = IWL_INVALID_VALUE;
	u32 fail_count;
	enum rs_action scale_action = RS_ACTION_STAY;
	u16 rate_mask;
	u8 update_lq = 0;
	struct iwl_scale_tbl_info *tbl, *tbl1;
	u8 active_tbl = 0;
	u8 done_search = 0;
	u16 high_low;
	s32 sr;
	u8 tid = IWL_MAX_TID_COUNT;
	u8 prev_agg = lq_sta->is_agg;
	struct iwl_mvm_sta *sta_priv = (void *)sta->drv_priv;
	struct iwl_mvm_tid_data *tid_data;
	struct rs_rate *rate;

	/* Send management frames and NO_ACK data using lowest rate. */
	/* TODO: this could probably be improved.. */
	if (!ieee80211_is_data(hdr->frame_control) ||
	    info->flags & IEEE80211_TX_CTL_NO_ACK)
		return;

	tid = rs_get_tid(lq_sta, hdr);
	if ((tid != IWL_MAX_TID_COUNT) &&
	    (lq_sta->tx_agg_tid_en & (1 << tid))) {
		tid_data = &sta_priv->tid_data[tid];
		if (tid_data->state == IWL_AGG_OFF)
			lq_sta->is_agg = 0;
		else
			lq_sta->is_agg = 1;
	} else {
		lq_sta->is_agg = 0;
	}

	/*
	 * Select rate-scale / modulation-mode table to work with in
	 * the rest of this function:  "search" if searching for better
	 * modulation mode, or "active" if doing rate scaling within a mode.
	 */
	if (!lq_sta->search_better_tbl)
		active_tbl = lq_sta->active_tbl;
	else
		active_tbl = 1 - lq_sta->active_tbl;

	tbl = &(lq_sta->lq_info[active_tbl]);
	rate = &tbl->rate;

	if (prev_agg != lq_sta->is_agg) {
		IWL_DEBUG_RATE(mvm,
			       "Aggregation changed: prev %d current %d. Update expected TPT table\n",
			       prev_agg, lq_sta->is_agg);
		rs_set_expected_tpt_table(lq_sta, tbl);
		rs_rate_scale_clear_tbl_windows(mvm, tbl);
	}

	/* current tx rate */
	index = lq_sta->last_txrate_idx;

	/* rates available for this association, and for modulation mode */
	rate_mask = rs_get_supported_rates(lq_sta, rate);

	if (!(BIT(index) & rate_mask)) {
		IWL_ERR(mvm, "Current Rate is not valid\n");
		if (lq_sta->search_better_tbl) {
			/* revert to active table if search table is not valid*/
			rate->type = LQ_NONE;
			lq_sta->search_better_tbl = 0;
			tbl = &(lq_sta->lq_info[lq_sta->active_tbl]);
			rs_update_rate_tbl(mvm, sta, lq_sta, &tbl->rate);
		}
		return;
	}

	/* Get expected throughput table and history window for current rate */
	if (!tbl->expected_tpt) {
		IWL_ERR(mvm, "tbl->expected_tpt is NULL\n");
		return;
	}

	/* force user max rate if set by user */
	if ((lq_sta->max_rate_idx != -1) &&
	    (lq_sta->max_rate_idx < index)) {
		index = lq_sta->max_rate_idx;
		update_lq = 1;
		window = &(tbl->win[index]);
		IWL_DEBUG_RATE(mvm,
			       "Forcing user max rate %d\n",
			       index);
		goto lq_update;
	}

	window = &(tbl->win[index]);

	/*
	 * If there is not enough history to calculate actual average
	 * throughput, keep analyzing results of more tx frames, without
	 * changing rate or mode (bypass most of the rest of this function).
	 * Set up new rate table in uCode only if old rate is not supported
	 * in current association (use new rate found above).
	 */
	fail_count = window->counter - window->success_counter;
	if ((fail_count < IWL_RATE_MIN_FAILURE_TH) &&
	    (window->success_counter < IWL_RATE_MIN_SUCCESS_TH)) {
		IWL_DEBUG_RATE(mvm,
			       "(%s: %d): Test Window: succ %d total %d\n",
			       rs_pretty_lq_type(rate->type),
			       index, window->success_counter, window->counter);

		/* Can't calculate this yet; not enough history */
		window->average_tpt = IWL_INVALID_VALUE;

		/* Should we stay with this modulation mode,
		 * or search for a new one? */
		rs_stay_in_table(lq_sta, false);

		goto out;
	}
	/* Else we have enough samples; calculate estimate of
	 * actual average throughput */
	if (window->average_tpt != ((window->success_ratio *
			tbl->expected_tpt[index] + 64) / 128)) {
		window->average_tpt = ((window->success_ratio *
					tbl->expected_tpt[index] + 64) / 128);
	}

	/* If we are searching for better modulation mode, check success. */
	if (lq_sta->search_better_tbl) {
		/* If good success, continue using the "search" mode;
		 * no need to send new link quality command, since we're
		 * continuing to use the setup that we've been trying. */
		if (window->average_tpt > lq_sta->last_tpt) {
			IWL_DEBUG_RATE(mvm,
				       "SWITCHING TO NEW TABLE SR: %d "
				       "cur-tpt %d old-tpt %d\n",
				       window->success_ratio,
				       window->average_tpt,
				       lq_sta->last_tpt);

			/* Swap tables; "search" becomes "active" */
			lq_sta->active_tbl = active_tbl;
			current_tpt = window->average_tpt;
		/* Else poor success; go back to mode in "active" table */
		} else {
			IWL_DEBUG_RATE(mvm,
				       "GOING BACK TO THE OLD TABLE: SR %d "
				       "cur-tpt %d old-tpt %d\n",
				       window->success_ratio,
				       window->average_tpt,
				       lq_sta->last_tpt);

			/* Nullify "search" table */
			rate->type = LQ_NONE;

			/* Revert to "active" table */
			active_tbl = lq_sta->active_tbl;
			tbl = &(lq_sta->lq_info[active_tbl]);

			/* Revert to "active" rate and throughput info */
			index = tbl->rate.index;
			current_tpt = lq_sta->last_tpt;

			/* Need to set up a new rate table in uCode */
			update_lq = 1;
		}

		/* Either way, we've made a decision; modulation mode
		 * search is done, allow rate adjustment next time. */
		lq_sta->search_better_tbl = 0;
		done_search = 1;	/* Don't switch modes below! */
		goto lq_update;
	}

	/* (Else) not in search of better modulation mode, try for better
	 * starting rate, while staying in this mode. */
	high_low = rs_get_adjacent_rate(mvm, index, rate_mask, rate->type);
	low = high_low & 0xff;
	high = (high_low >> 8) & 0xff;

	/* If user set max rate, dont allow higher than user constrain */
	if ((lq_sta->max_rate_idx != -1) &&
	    (lq_sta->max_rate_idx < high))
		high = IWL_RATE_INVALID;

	sr = window->success_ratio;

	/* Collect measured throughputs for current and adjacent rates */
	current_tpt = window->average_tpt;
	if (low != IWL_RATE_INVALID)
		low_tpt = tbl->win[low].average_tpt;
	if (high != IWL_RATE_INVALID)
		high_tpt = tbl->win[high].average_tpt;

	IWL_DEBUG_RATE(mvm,
		       "(%s: %d): cur_tpt %d SR %d low %d high %d low_tpt %d high_tpt %d\n",
		       rs_pretty_lq_type(rate->type), index, current_tpt, sr,
		       low, high, low_tpt, high_tpt);

	scale_action = rs_get_rate_action(mvm, tbl, sr, low, high,
					  current_tpt, low_tpt, high_tpt);

	/* Force a search in case BT doesn't like us being in MIMO */
	if (is_mimo(rate) &&
	    !iwl_mvm_bt_coex_is_mimo_allowed(mvm, sta)) {
		IWL_DEBUG_RATE(mvm,
			       "BT Coex forbids MIMO. Search for new config\n");
		rs_stay_in_table(lq_sta, true);
		goto lq_update;
	}

	switch (scale_action) {
	case RS_ACTION_DOWNSCALE:
		/* Decrease starting rate, update uCode's rate table */
		if (low != IWL_RATE_INVALID) {
			update_lq = 1;
			index = low;
		} else {
			IWL_DEBUG_RATE(mvm,
				       "At the bottom rate. Can't decrease\n");
		}

		break;
	case RS_ACTION_UPSCALE:
		/* Increase starting rate, update uCode's rate table */
		if (high != IWL_RATE_INVALID) {
			update_lq = 1;
			index = high;
		} else {
			IWL_DEBUG_RATE(mvm,
				       "At the top rate. Can't increase\n");
		}

		break;
	case RS_ACTION_STAY:
		/* No change */
		if (lq_sta->rs_state == RS_STATE_STAY_IN_COLUMN)
			update_lq = rs_tpc_perform(mvm, sta, lq_sta, tbl);
		break;
	default:
		break;
	}

lq_update:
	/* Replace uCode's rate table for the destination station. */
	if (update_lq) {
		tbl->rate.index = index;
		rs_update_rate_tbl(mvm, sta, lq_sta, &tbl->rate);
	}

	rs_stay_in_table(lq_sta, false);

	/*
	 * Search for new modulation mode if we're:
	 * 1)  Not changing rates right now
	 * 2)  Not just finishing up a search
	 * 3)  Allowing a new search
	 */
	if (!update_lq && !done_search &&
	    lq_sta->rs_state == RS_STATE_SEARCH_CYCLE_STARTED
	    && window->counter) {
		enum rs_column next_column;

		/* Save current throughput to compare with "search" throughput*/
		lq_sta->last_tpt = current_tpt;

		IWL_DEBUG_RATE(mvm,
			       "Start Search: update_lq %d done_search %d rs_state %d win->counter %d\n",
			       update_lq, done_search, lq_sta->rs_state,
			       window->counter);

		next_column = rs_get_next_column(mvm, lq_sta, sta, tbl);
		if (next_column != RS_COLUMN_INVALID) {
			int ret = rs_switch_to_column(mvm, lq_sta, sta,
						      next_column);
			if (!ret)
				lq_sta->search_better_tbl = 1;
		} else {
			IWL_DEBUG_RATE(mvm,
				       "No more columns to explore in search cycle. Go to RS_STATE_SEARCH_CYCLE_ENDED\n");
			lq_sta->rs_state = RS_STATE_SEARCH_CYCLE_ENDED;
		}

		/* If new "search" mode was selected, set up in uCode table */
		if (lq_sta->search_better_tbl) {
			/* Access the "search" table, clear its history. */
			tbl = &(lq_sta->lq_info[(1 - lq_sta->active_tbl)]);
			rs_rate_scale_clear_tbl_windows(mvm, tbl);

			/* Use new "search" start rate */
			index = tbl->rate.index;

			rs_dump_rate(mvm, &tbl->rate,
				     "Switch to SEARCH TABLE:");
			rs_fill_lq_cmd(mvm, sta, lq_sta, &tbl->rate);
			iwl_mvm_send_lq_cmd(mvm, &lq_sta->lq, false);
		} else {
			done_search = 1;
		}
	}

	if (done_search && lq_sta->rs_state == RS_STATE_SEARCH_CYCLE_ENDED) {
		/* If the "active" (non-search) mode was legacy,
		 * and we've tried switching antennas,
		 * but we haven't been able to try HT modes (not available),
		 * stay with best antenna legacy modulation for a while
		 * before next round of mode comparisons. */
		tbl1 = &(lq_sta->lq_info[lq_sta->active_tbl]);
		if (is_legacy(&tbl1->rate)) {
			IWL_DEBUG_RATE(mvm, "LQ: STAY in legacy table\n");

			if (tid != IWL_MAX_TID_COUNT) {
				tid_data = &sta_priv->tid_data[tid];
				if (tid_data->state != IWL_AGG_OFF) {
					IWL_DEBUG_RATE(mvm,
						       "Stop aggregation on tid %d\n",
						       tid);
					ieee80211_stop_tx_ba_session(sta, tid);
				}
			}
			rs_set_stay_in_table(mvm, 1, lq_sta);
		} else {
		/* If we're in an HT mode, and all 3 mode switch actions
		 * have been tried and compared, stay in this best modulation
		 * mode for a while before next round of mode comparisons. */
			if ((lq_sta->last_tpt > IWL_AGG_TPT_THREHOLD) &&
			    (lq_sta->tx_agg_tid_en & (1 << tid)) &&
			    (tid != IWL_MAX_TID_COUNT)) {
				tid_data = &sta_priv->tid_data[tid];
				if (tid_data->state == IWL_AGG_OFF) {
					IWL_DEBUG_RATE(mvm,
						       "try to aggregate tid %d\n",
						       tid);
					rs_tl_turn_on_agg(mvm, tid,
							  lq_sta, sta);
				}
			}
			rs_set_stay_in_table(mvm, 0, lq_sta);
		}
	}

out:
	lq_sta->last_txrate_idx = index;
}

struct rs_init_rate_info {
	s8 rssi;
	u8 rate_idx;
};

static const struct rs_init_rate_info rs_init_rates_24ghz[] = {
	{ -60, IWL_RATE_54M_INDEX },
	{ -64, IWL_RATE_48M_INDEX },
	{ -68, IWL_RATE_36M_INDEX },
	{ -80, IWL_RATE_24M_INDEX },
	{ -84, IWL_RATE_18M_INDEX },
	{ -85, IWL_RATE_12M_INDEX },
	{ -86, IWL_RATE_11M_INDEX },
	{ -88, IWL_RATE_5M_INDEX  },
	{ -90, IWL_RATE_2M_INDEX  },
	{ S8_MIN, IWL_RATE_1M_INDEX },
};

static const struct rs_init_rate_info rs_init_rates_5ghz[] = {
	{ -60, IWL_RATE_54M_INDEX },
	{ -64, IWL_RATE_48M_INDEX },
	{ -72, IWL_RATE_36M_INDEX },
	{ -80, IWL_RATE_24M_INDEX },
	{ -84, IWL_RATE_18M_INDEX },
	{ -85, IWL_RATE_12M_INDEX },
	{ -87, IWL_RATE_9M_INDEX  },
	{ S8_MIN, IWL_RATE_6M_INDEX },
};

/* Choose an initial legacy rate and antenna to use based on the RSSI
 * of last Rx
 */
static void rs_get_initial_rate(struct iwl_mvm *mvm,
				struct iwl_lq_sta *lq_sta,
				enum ieee80211_band band,
				struct rs_rate *rate)
{
	int i, nentries;
	s8 best_rssi = S8_MIN;
	u8 best_ant = ANT_NONE;
	u8 valid_tx_ant = mvm->fw->valid_tx_ant;
	const struct rs_init_rate_info *initial_rates;

	for (i = 0; i < ARRAY_SIZE(lq_sta->pers.chain_signal); i++) {
		if (!(lq_sta->pers.chains & BIT(i)))
			continue;

		if (lq_sta->pers.chain_signal[i] > best_rssi) {
			best_rssi = lq_sta->pers.chain_signal[i];
			best_ant = BIT(i);
		}
	}

	IWL_DEBUG_RATE(mvm, "Best ANT: %s Best RSSI: %d\n",
		       rs_pretty_ant(best_ant), best_rssi);

	if (best_ant != ANT_A && best_ant != ANT_B)
		rate->ant = first_antenna(valid_tx_ant);
	else
		rate->ant = best_ant;

	rate->sgi = false;
	rate->ldpc = false;
	rate->bw = RATE_MCS_CHAN_WIDTH_20;

	if (band == IEEE80211_BAND_5GHZ) {
		rate->type = LQ_LEGACY_A;
		rate->index = IWL_RATE_6M_INDEX;
		initial_rates = rs_init_rates_5ghz;
		nentries = ARRAY_SIZE(rs_init_rates_5ghz);
	} else {
		rate->type = LQ_LEGACY_G;
		rate->index = IWL_RATE_1M_INDEX;
		initial_rates = rs_init_rates_24ghz;
		nentries = ARRAY_SIZE(rs_init_rates_24ghz);
	}

	for (i = 0; i < nentries; i++) {
		int rate_idx = initial_rates[i].rate_idx;
		if ((best_rssi >= initial_rates[i].rssi) &&
		    (BIT(rate_idx) & lq_sta->active_legacy_rate)) {
			rate->index = rate_idx;
			break;
		}
	}

	IWL_DEBUG_RATE(mvm, "rate_idx %d ANT %s\n", rate->index,
		       rs_pretty_ant(rate->ant));
}

/* Save info about RSSI of last Rx */
void rs_update_last_rssi(struct iwl_mvm *mvm,
			 struct iwl_lq_sta *lq_sta,
			 struct ieee80211_rx_status *rx_status)
{
	lq_sta->pers.chains = rx_status->chains;
	lq_sta->pers.chain_signal[0] = rx_status->chain_signal[0];
	lq_sta->pers.chain_signal[1] = rx_status->chain_signal[1];
	lq_sta->pers.chain_signal[2] = rx_status->chain_signal[2];
}

/**
 * rs_initialize_lq - Initialize a station's hardware rate table
 *
 * The uCode's station table contains a table of fallback rates
 * for automatic fallback during transmission.
 *
 * NOTE: This sets up a default set of values.  These will be replaced later
 *       if the driver's iwl-agn-rs rate scaling algorithm is used, instead of
 *       rc80211_simple.
 *
 * NOTE: Run REPLY_ADD_STA command to set up station table entry, before
 *       calling this function (which runs REPLY_TX_LINK_QUALITY_CMD,
 *       which requires station table entry to exist).
 */
static void rs_initialize_lq(struct iwl_mvm *mvm,
			     struct ieee80211_sta *sta,
			     struct iwl_lq_sta *lq_sta,
			     enum ieee80211_band band,
			     bool init)
{
	struct iwl_scale_tbl_info *tbl;
	struct rs_rate *rate;
	u8 active_tbl = 0;

	if (!sta || !lq_sta)
		return;

	if (!lq_sta->search_better_tbl)
		active_tbl = lq_sta->active_tbl;
	else
		active_tbl = 1 - lq_sta->active_tbl;

	tbl = &(lq_sta->lq_info[active_tbl]);
	rate = &tbl->rate;

	rs_get_initial_rate(mvm, lq_sta, band, rate);
	lq_sta->last_txrate_idx = rate->index;

	WARN_ON_ONCE(rate->ant != ANT_A && rate->ant != ANT_B);
	if (rate->ant == ANT_A)
		tbl->column = RS_COLUMN_LEGACY_ANT_A;
	else
		tbl->column = RS_COLUMN_LEGACY_ANT_B;

	rs_set_expected_tpt_table(lq_sta, tbl);
	rs_fill_lq_cmd(mvm, sta, lq_sta, rate);
	/* TODO restore station should remember the lq cmd */
	iwl_mvm_send_lq_cmd(mvm, &lq_sta->lq, init);
}

static void rs_get_rate(void *mvm_r, struct ieee80211_sta *sta, void *mvm_sta,
			struct ieee80211_tx_rate_control *txrc)
{
	struct sk_buff *skb = txrc->skb;
	struct ieee80211_supported_band *sband = txrc->sband;
	struct iwl_op_mode *op_mode __maybe_unused =
			(struct iwl_op_mode *)mvm_r;
	struct iwl_mvm *mvm __maybe_unused = IWL_OP_MODE_GET_MVM(op_mode);
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct iwl_lq_sta *lq_sta = mvm_sta;

	/* Get max rate if user set max rate */
	if (lq_sta) {
		lq_sta->max_rate_idx = txrc->max_rate_idx;
		if ((sband->band == IEEE80211_BAND_5GHZ) &&
		    (lq_sta->max_rate_idx != -1))
			lq_sta->max_rate_idx += IWL_FIRST_OFDM_RATE;
		if ((lq_sta->max_rate_idx < 0) ||
		    (lq_sta->max_rate_idx >= IWL_RATE_COUNT))
			lq_sta->max_rate_idx = -1;
	}

	/* Treat uninitialized rate scaling data same as non-existing. */
	if (lq_sta && !lq_sta->pers.drv) {
		IWL_DEBUG_RATE(mvm, "Rate scaling not initialized yet.\n");
		mvm_sta = NULL;
	}

	/* Send management frames and NO_ACK data using lowest rate. */
	if (rate_control_send_low(sta, mvm_sta, txrc))
		return;

	iwl_mvm_hwrate_to_tx_rate(lq_sta->last_rate_n_flags,
				  info->band, &info->control.rates[0]);

	info->control.rates[0].count = 1;
}

static void *rs_alloc_sta(void *mvm_rate, struct ieee80211_sta *sta,
			  gfp_t gfp)
{
	struct iwl_mvm_sta *sta_priv = (struct iwl_mvm_sta *)sta->drv_priv;
	struct iwl_op_mode *op_mode = (struct iwl_op_mode *)mvm_rate;
	struct iwl_mvm *mvm  = IWL_OP_MODE_GET_MVM(op_mode);
	struct iwl_lq_sta *lq_sta = &sta_priv->lq_sta;

	IWL_DEBUG_RATE(mvm, "create station rate scale window\n");

	lq_sta->pers.drv = mvm;
#ifdef CPTCFG_MAC80211_DEBUGFS
	lq_sta->pers.dbg_fixed_rate = 0;
	lq_sta->pers.dbg_fixed_txp_reduction = TPC_INVALID;
#endif
	lq_sta->pers.chains = 0;
	memset(lq_sta->pers.chain_signal, 0, sizeof(lq_sta->pers.chain_signal));

	return &sta_priv->lq_sta;
}

static int rs_vht_highest_rx_mcs_index(struct ieee80211_sta_vht_cap *vht_cap,
				       int nss)
{
	u16 rx_mcs = le16_to_cpu(vht_cap->vht_mcs.rx_mcs_map) &
		(0x3 << (2 * (nss - 1)));
	rx_mcs >>= (2 * (nss - 1));

	if (rx_mcs == IEEE80211_VHT_MCS_SUPPORT_0_7)
		return IWL_RATE_MCS_7_INDEX;
	else if (rx_mcs == IEEE80211_VHT_MCS_SUPPORT_0_8)
		return IWL_RATE_MCS_8_INDEX;
	else if (rx_mcs == IEEE80211_VHT_MCS_SUPPORT_0_9)
		return IWL_RATE_MCS_9_INDEX;

	WARN_ON_ONCE(rx_mcs != IEEE80211_VHT_MCS_NOT_SUPPORTED);
	return -1;
}

static void rs_vht_set_enabled_rates(struct ieee80211_sta *sta,
				     struct ieee80211_sta_vht_cap *vht_cap,
				     struct iwl_lq_sta *lq_sta)
{
	int i;
	int highest_mcs = rs_vht_highest_rx_mcs_index(vht_cap, 1);

	if (highest_mcs >= IWL_RATE_MCS_0_INDEX) {
		for (i = IWL_RATE_MCS_0_INDEX; i <= highest_mcs; i++) {
			if (i == IWL_RATE_9M_INDEX)
				continue;

			/* VHT MCS9 isn't valid for 20Mhz for NSS=1,2 */
			if (i == IWL_RATE_MCS_9_INDEX &&
			    sta->bandwidth == IEEE80211_STA_RX_BW_20)
				continue;

			lq_sta->active_siso_rate |= BIT(i);
		}
	}

	if (sta->rx_nss < 2)
		return;

	highest_mcs = rs_vht_highest_rx_mcs_index(vht_cap, 2);
	if (highest_mcs >= IWL_RATE_MCS_0_INDEX) {
		for (i = IWL_RATE_MCS_0_INDEX; i <= highest_mcs; i++) {
			if (i == IWL_RATE_9M_INDEX)
				continue;

			/* VHT MCS9 isn't valid for 20Mhz for NSS=1,2 */
			if (i == IWL_RATE_MCS_9_INDEX &&
			    sta->bandwidth == IEEE80211_STA_RX_BW_20)
				continue;

			lq_sta->active_mimo2_rate |= BIT(i);
		}
	}
}

#ifdef CPTCFG_IWLWIFI_DEBUGFS
static void iwl_mvm_reset_frame_stats(struct iwl_mvm *mvm,
				      struct iwl_mvm_frame_stats *stats)
{
	spin_lock_bh(&mvm->drv_stats_lock);
	memset(stats, 0, sizeof(*stats));
	spin_unlock_bh(&mvm->drv_stats_lock);
}

void iwl_mvm_update_frame_stats(struct iwl_mvm *mvm,
				struct iwl_mvm_frame_stats *stats,
				u32 rate, bool agg)
{
	u8 nss = 0, mcs = 0;

	spin_lock(&mvm->drv_stats_lock);

	if (agg)
		stats->agg_frames++;

	stats->success_frames++;

	switch (rate & RATE_MCS_CHAN_WIDTH_MSK) {
	case RATE_MCS_CHAN_WIDTH_20:
		stats->bw_20_frames++;
		break;
	case RATE_MCS_CHAN_WIDTH_40:
		stats->bw_40_frames++;
		break;
	case RATE_MCS_CHAN_WIDTH_80:
		stats->bw_80_frames++;
		break;
	default:
		WARN_ONCE(1, "bad BW. rate 0x%x", rate);
	}

	if (rate & RATE_MCS_HT_MSK) {
		stats->ht_frames++;
		mcs = rate & RATE_HT_MCS_RATE_CODE_MSK;
		nss = ((rate & RATE_HT_MCS_NSS_MSK) >> RATE_HT_MCS_NSS_POS) + 1;
	} else if (rate & RATE_MCS_VHT_MSK) {
		stats->vht_frames++;
		mcs = rate & RATE_VHT_MCS_RATE_CODE_MSK;
		nss = ((rate & RATE_VHT_MCS_NSS_MSK) >>
		       RATE_VHT_MCS_NSS_POS) + 1;
	} else {
		stats->legacy_frames++;
	}

	if (nss == 1)
		stats->siso_frames++;
	else if (nss == 2)
		stats->mimo2_frames++;

	if (rate & RATE_MCS_SGI_MSK)
		stats->sgi_frames++;
	else
		stats->ngi_frames++;

	stats->last_rates[stats->last_frame_idx] = rate;
	stats->last_frame_idx = (stats->last_frame_idx + 1) %
		ARRAY_SIZE(stats->last_rates);

	spin_unlock(&mvm->drv_stats_lock);
}
#endif

/*
 * Called after adding a new station to initialize rate scaling
 */
void iwl_mvm_rs_rate_init(struct iwl_mvm *mvm, struct ieee80211_sta *sta,
			  enum ieee80211_band band, bool init)
{
	int i, j;
	struct ieee80211_hw *hw = mvm->hw;
	struct ieee80211_sta_ht_cap *ht_cap = &sta->ht_cap;
	struct ieee80211_sta_vht_cap *vht_cap = &sta->vht_cap;
	struct iwl_mvm_sta *sta_priv;
	struct iwl_lq_sta *lq_sta;
	struct ieee80211_supported_band *sband;
	unsigned long supp; /* must be unsigned long for for_each_set_bit */

	sta_priv = (struct iwl_mvm_sta *)sta->drv_priv;
	lq_sta = &sta_priv->lq_sta;

	/* clear all non-persistent lq data */
	memset(lq_sta, 0, offsetof(typeof(*lq_sta), pers));

	sband = hw->wiphy->bands[band];

	lq_sta->lq.sta_id = sta_priv->sta_id;

	for (j = 0; j < LQ_SIZE; j++)
		rs_rate_scale_clear_tbl_windows(mvm, &lq_sta->lq_info[j]);

	lq_sta->flush_timer = 0;
	lq_sta->last_tx = jiffies;

	IWL_DEBUG_RATE(mvm,
		       "LQ: *** rate scale station global init for station %d ***\n",
		       sta_priv->sta_id);
	/* TODO: what is a good starting rate for STA? About middle? Maybe not
	 * the lowest or the highest rate.. Could consider using RSSI from
	 * previous packets? Need to have IEEE 802.1X auth succeed immediately
	 * after assoc.. */

	lq_sta->max_rate_idx = -1;
	lq_sta->missed_rate_counter = IWL_MISSED_RATE_MAX;
	lq_sta->band = sband->band;
	/*
	 * active legacy rates as per supported rates bitmap
	 */
	supp = sta->supp_rates[sband->band];
	lq_sta->active_legacy_rate = 0;
	for_each_set_bit(i, &supp, BITS_PER_LONG)
		lq_sta->active_legacy_rate |= BIT(sband->bitrates[i].hw_value);

	/* TODO: should probably account for rx_highest for both HT/VHT */
	if (!vht_cap || !vht_cap->vht_supported) {
		/* active_siso_rate mask includes 9 MBits (bit 5),
		 * and CCK (bits 0-3), supp_rates[] does not;
		 * shift to convert format, force 9 MBits off.
		 */
		lq_sta->active_siso_rate = ht_cap->mcs.rx_mask[0] << 1;
		lq_sta->active_siso_rate |= ht_cap->mcs.rx_mask[0] & 0x1;
		lq_sta->active_siso_rate &= ~((u16)0x2);
		lq_sta->active_siso_rate <<= IWL_FIRST_OFDM_RATE;

		/* Same here */
		lq_sta->active_mimo2_rate = ht_cap->mcs.rx_mask[1] << 1;
		lq_sta->active_mimo2_rate |= ht_cap->mcs.rx_mask[1] & 0x1;
		lq_sta->active_mimo2_rate &= ~((u16)0x2);
		lq_sta->active_mimo2_rate <<= IWL_FIRST_OFDM_RATE;

		lq_sta->is_vht = false;
		if (mvm->cfg->ht_params->ldpc &&
		    (ht_cap->cap & IEEE80211_HT_CAP_LDPC_CODING))
			lq_sta->ldpc = true;
	} else {
		rs_vht_set_enabled_rates(sta, vht_cap, lq_sta);
		lq_sta->is_vht = true;

		if (mvm->cfg->ht_params->ldpc &&
		    (vht_cap->cap & IEEE80211_VHT_CAP_RXLDPC))
			lq_sta->ldpc = true;
	}

	lq_sta->max_legacy_rate_idx = find_last_bit(&lq_sta->active_legacy_rate,
						    BITS_PER_LONG);
	lq_sta->max_siso_rate_idx = find_last_bit(&lq_sta->active_siso_rate,
						  BITS_PER_LONG);
	lq_sta->max_mimo2_rate_idx = find_last_bit(&lq_sta->active_mimo2_rate,
						   BITS_PER_LONG);

	IWL_DEBUG_RATE(mvm,
		       "RATE MASK: LEGACY=%lX SISO=%lX MIMO2=%lX VHT=%d LDPC=%d\n",
		       lq_sta->active_legacy_rate,
		       lq_sta->active_siso_rate,
		       lq_sta->active_mimo2_rate,
		       lq_sta->is_vht, lq_sta->ldpc);
	IWL_DEBUG_RATE(mvm, "MAX RATE: LEGACY=%d SISO=%d MIMO2=%d\n",
		       lq_sta->max_legacy_rate_idx,
		       lq_sta->max_siso_rate_idx,
		       lq_sta->max_mimo2_rate_idx);

	/* These values will be overridden later */
	lq_sta->lq.single_stream_ant_msk =
		first_antenna(mvm->fw->valid_tx_ant);
	lq_sta->lq.dual_stream_ant_msk = ANT_AB;

	/* as default allow aggregation for all tids */
	lq_sta->tx_agg_tid_en = IWL_AGG_ALL_TID;
	lq_sta->is_agg = 0;
#ifdef CPTCFG_IWLWIFI_DEBUGFS
	iwl_mvm_reset_frame_stats(mvm, &mvm->drv_rx_stats);
#endif
	rs_initialize_lq(mvm, sta, lq_sta, band, init);
}

static void rs_rate_update(void *mvm_r,
			   struct ieee80211_supported_band *sband,
			   struct cfg80211_chan_def *chandef,
			   struct ieee80211_sta *sta, void *priv_sta,
			   u32 changed)
{
	u8 tid;
	struct iwl_op_mode *op_mode  =
			(struct iwl_op_mode *)mvm_r;
	struct iwl_mvm *mvm = IWL_OP_MODE_GET_MVM(op_mode);

	/* Stop any ongoing aggregations as rs starts off assuming no agg */
	for (tid = 0; tid < IWL_MAX_TID_COUNT; tid++)
		ieee80211_stop_tx_ba_session(sta, tid);

	iwl_mvm_rs_rate_init(mvm, sta, sband->band, false);
}

#ifdef CPTCFG_MAC80211_DEBUGFS
static void rs_build_rates_table_from_fixed(struct iwl_mvm *mvm,
					    struct iwl_lq_cmd *lq_cmd,
					    enum ieee80211_band band,
					    u32 ucode_rate)
{
	struct rs_rate rate;
	int i;
	int num_rates = ARRAY_SIZE(lq_cmd->rs_table);
	__le32 ucode_rate_le32 = cpu_to_le32(ucode_rate);

	for (i = 0; i < num_rates; i++)
		lq_cmd->rs_table[i] = ucode_rate_le32;

	rs_rate_from_ucode_rate(ucode_rate, band, &rate);

	if (is_mimo(&rate))
		lq_cmd->mimo_delim = num_rates - 1;
	else
		lq_cmd->mimo_delim = 0;
}
#endif /* CPTCFG_MAC80211_DEBUGFS */

static void rs_fill_rates_for_column(struct iwl_mvm *mvm,
				     struct iwl_lq_sta *lq_sta,
				     struct rs_rate *rate,
				     __le32 *rs_table, int *rs_table_index,
				     int num_rates, int num_retries,
				     u8 valid_tx_ant, bool toggle_ant)
{
	int i, j;
	__le32 ucode_rate;
	bool bottom_reached = false;
	int prev_rate_idx = rate->index;
	int end = LINK_QUAL_MAX_RETRY_NUM;
	int index = *rs_table_index;

	for (i = 0; i < num_rates && index < end; i++) {
		ucode_rate = cpu_to_le32(ucode_rate_from_rs_rate(mvm, rate));
		for (j = 0; j < num_retries && index < end; j++, index++)
			rs_table[index] = ucode_rate;

		if (toggle_ant)
			rs_toggle_antenna(valid_tx_ant, rate);

		prev_rate_idx = rate->index;
		bottom_reached = rs_get_lower_rate_in_column(lq_sta, rate);
		if (bottom_reached && !is_legacy(rate))
			break;
	}

	if (!bottom_reached)
		rate->index = prev_rate_idx;

	*rs_table_index = index;
}

/* Building the rate table is non trivial. When we're in MIMO2/VHT/80Mhz/SGI
 * column the rate table should look like this:
 *
 * rate[0] 0x400D019 VHT | ANT: AB BW: 80Mhz MCS: 9 NSS: 2 SGI
 * rate[1] 0x400D019 VHT | ANT: AB BW: 80Mhz MCS: 9 NSS: 2 SGI
 * rate[2] 0x400D018 VHT | ANT: AB BW: 80Mhz MCS: 8 NSS: 2 SGI
 * rate[3] 0x400D018 VHT | ANT: AB BW: 80Mhz MCS: 8 NSS: 2 SGI
 * rate[4] 0x400D017 VHT | ANT: AB BW: 80Mhz MCS: 7 NSS: 2 SGI
 * rate[5] 0x400D017 VHT | ANT: AB BW: 80Mhz MCS: 7 NSS: 2 SGI
 * rate[6] 0x4005007 VHT | ANT: A BW: 80Mhz MCS: 7 NSS: 1 NGI
 * rate[7] 0x4009006 VHT | ANT: B BW: 80Mhz MCS: 6 NSS: 1 NGI
 * rate[8] 0x4005005 VHT | ANT: A BW: 80Mhz MCS: 5 NSS: 1 NGI
 * rate[9] 0x800B Legacy | ANT: B Rate: 36 Mbps
 * rate[10] 0x4009 Legacy | ANT: A Rate: 24 Mbps
 * rate[11] 0x8007 Legacy | ANT: B Rate: 18 Mbps
 * rate[12] 0x4005 Legacy | ANT: A Rate: 12 Mbps
 * rate[13] 0x800F Legacy | ANT: B Rate: 9 Mbps
 * rate[14] 0x400D Legacy | ANT: A Rate: 6 Mbps
 * rate[15] 0x800D Legacy | ANT: B Rate: 6 Mbps
 */
static void rs_build_rates_table(struct iwl_mvm *mvm,
				 struct iwl_lq_sta *lq_sta,
				 const struct rs_rate *initial_rate)
{
	struct rs_rate rate;
	int num_rates, num_retries, index = 0;
	u8 valid_tx_ant = 0;
	struct iwl_lq_cmd *lq_cmd = &lq_sta->lq;
	bool toggle_ant = false;

	memcpy(&rate, initial_rate, sizeof(rate));

	valid_tx_ant = mvm->fw->valid_tx_ant;

	if (is_siso(&rate)) {
		num_rates = RS_INITIAL_SISO_NUM_RATES;
		num_retries = RS_HT_VHT_RETRIES_PER_RATE;
	} else if (is_mimo(&rate)) {
		num_rates = RS_INITIAL_MIMO_NUM_RATES;
		num_retries = RS_HT_VHT_RETRIES_PER_RATE;
	} else {
		num_rates = RS_INITIAL_LEGACY_NUM_RATES;
		num_retries = RS_LEGACY_RETRIES_PER_RATE;
		toggle_ant = true;
	}

	rs_fill_rates_for_column(mvm, lq_sta, &rate, lq_cmd->rs_table, &index,
				 num_rates, num_retries, valid_tx_ant,
				 toggle_ant);

	rs_get_lower_rate_down_column(lq_sta, &rate);

	if (is_siso(&rate)) {
		num_rates = RS_SECONDARY_SISO_NUM_RATES;
		num_retries = RS_SECONDARY_SISO_RETRIES;
		lq_cmd->mimo_delim = index;
	} else if (is_legacy(&rate)) {
		num_rates = RS_SECONDARY_LEGACY_NUM_RATES;
		num_retries = RS_LEGACY_RETRIES_PER_RATE;
	} else {
		WARN_ON_ONCE(1);
	}

	toggle_ant = true;

	rs_fill_rates_for_column(mvm, lq_sta, &rate, lq_cmd->rs_table, &index,
				 num_rates, num_retries, valid_tx_ant,
				 toggle_ant);

	rs_get_lower_rate_down_column(lq_sta, &rate);

	num_rates = RS_SECONDARY_LEGACY_NUM_RATES;
	num_retries = RS_LEGACY_RETRIES_PER_RATE;

	rs_fill_rates_for_column(mvm, lq_sta, &rate, lq_cmd->rs_table, &index,
				 num_rates, num_retries, valid_tx_ant,
				 toggle_ant);

}

static void rs_fill_lq_cmd(struct iwl_mvm *mvm,
			   struct ieee80211_sta *sta,
			   struct iwl_lq_sta *lq_sta,
			   const struct rs_rate *initial_rate)
{
	struct iwl_lq_cmd *lq_cmd = &lq_sta->lq;
	u8 ant = initial_rate->ant;

#ifdef CPTCFG_MAC80211_DEBUGFS
	if (lq_sta->pers.dbg_fixed_rate) {
		rs_build_rates_table_from_fixed(mvm, lq_cmd,
						lq_sta->band,
						lq_sta->pers.dbg_fixed_rate);
		lq_cmd->reduced_tpc = 0;
		ant = (lq_sta->pers.dbg_fixed_rate & RATE_MCS_ANT_ABC_MSK) >>
			RATE_MCS_ANT_POS;
	} else
#endif
		rs_build_rates_table(mvm, lq_sta, initial_rate);

	if (num_of_ant(ant) == 1)
		lq_cmd->single_stream_ant_msk = ant;

	lq_cmd->agg_frame_cnt_limit = LINK_QUAL_AGG_FRAME_LIMIT_DEF;
	lq_cmd->agg_disable_start_th = LINK_QUAL_AGG_DISABLE_START_DEF;

	lq_cmd->agg_time_limit =
		cpu_to_le16(LINK_QUAL_AGG_TIME_LIMIT_DEF);

	if (sta)
		lq_cmd->agg_time_limit =
			cpu_to_le16(iwl_mvm_coex_agg_time_limit(mvm, sta));
}

static void *rs_alloc(struct ieee80211_hw *hw, struct dentry *debugfsdir)
{
	return hw->priv;
}
/* rate scale requires free function to be implemented */
static void rs_free(void *mvm_rate)
{
	return;
}

static void rs_free_sta(void *mvm_r, struct ieee80211_sta *sta,
			void *mvm_sta)
{
	struct iwl_op_mode *op_mode __maybe_unused = mvm_r;
	struct iwl_mvm *mvm __maybe_unused = IWL_OP_MODE_GET_MVM(op_mode);

	IWL_DEBUG_RATE(mvm, "enter\n");
	IWL_DEBUG_RATE(mvm, "leave\n");
}

#ifdef CPTCFG_MAC80211_DEBUGFS
int rs_pretty_print_rate(char *buf, const u32 rate)
{

	char *type, *bw;
	u8 mcs = 0, nss = 0;
	u8 ant = (rate & RATE_MCS_ANT_ABC_MSK) >> RATE_MCS_ANT_POS;

	if (!(rate & RATE_MCS_HT_MSK) &&
	    !(rate & RATE_MCS_VHT_MSK)) {
		int index = iwl_hwrate_to_plcp_idx(rate);

		return sprintf(buf, "Legacy | ANT: %s Rate: %s Mbps\n",
			       rs_pretty_ant(ant),
			       index == IWL_RATE_INVALID ? "BAD" :
			       iwl_rate_mcs[index].mbps);
	}

	if (rate & RATE_MCS_VHT_MSK) {
		type = "VHT";
		mcs = rate & RATE_VHT_MCS_RATE_CODE_MSK;
		nss = ((rate & RATE_VHT_MCS_NSS_MSK)
		       >> RATE_VHT_MCS_NSS_POS) + 1;
	} else if (rate & RATE_MCS_HT_MSK) {
		type = "HT";
		mcs = rate & RATE_HT_MCS_INDEX_MSK;
	} else {
		type = "Unknown"; /* shouldn't happen */
	}

	switch (rate & RATE_MCS_CHAN_WIDTH_MSK) {
	case RATE_MCS_CHAN_WIDTH_20:
		bw = "20Mhz";
		break;
	case RATE_MCS_CHAN_WIDTH_40:
		bw = "40Mhz";
		break;
	case RATE_MCS_CHAN_WIDTH_80:
		bw = "80Mhz";
		break;
	case RATE_MCS_CHAN_WIDTH_160:
		bw = "160Mhz";
		break;
	default:
		bw = "BAD BW";
	}

	return sprintf(buf, "%s | ANT: %s BW: %s MCS: %d NSS: %d %s%s%s%s%s\n",
		       type, rs_pretty_ant(ant), bw, mcs, nss,
		       (rate & RATE_MCS_SGI_MSK) ? "SGI " : "NGI ",
		       (rate & RATE_MCS_HT_STBC_MSK) ? "STBC " : "",
		       (rate & RATE_MCS_LDPC_MSK) ? "LDPC " : "",
		       (rate & RATE_MCS_BF_MSK) ? "BF " : "",
		       (rate & RATE_MCS_ZLF_MSK) ? "ZLF " : "");
}

/**
 * Program the device to use fixed rate for frame transmit
 * This is for debugging/testing only
 * once the device start use fixed rate, we need to reload the module
 * to being back the normal operation.
 */
static void rs_program_fix_rate(struct iwl_mvm *mvm,
				struct iwl_lq_sta *lq_sta)
{
	lq_sta->active_legacy_rate = 0x0FFF;	/* 1 - 54 MBits, includes CCK */
	lq_sta->active_siso_rate   = 0x1FD0;	/* 6 - 60 MBits, no 9, no CCK */
	lq_sta->active_mimo2_rate  = 0x1FD0;	/* 6 - 60 MBits, no 9, no CCK */

	IWL_DEBUG_RATE(mvm, "sta_id %d rate 0x%X\n",
		       lq_sta->lq.sta_id, lq_sta->pers.dbg_fixed_rate);

	if (lq_sta->pers.dbg_fixed_rate) {
		struct rs_rate rate;
		rs_rate_from_ucode_rate(lq_sta->pers.dbg_fixed_rate,
					lq_sta->band, &rate);
		rs_fill_lq_cmd(mvm, NULL, lq_sta, &rate);
		iwl_mvm_send_lq_cmd(lq_sta->pers.drv, &lq_sta->lq, false);
	}
}

static ssize_t rs_sta_dbgfs_scale_table_write(struct file *file,
			const char __user *user_buf, size_t count, loff_t *ppos)
{
	struct iwl_lq_sta *lq_sta = file->private_data;
	struct iwl_mvm *mvm;
	char buf[64];
	size_t buf_size;
	u32 parsed_rate;

	mvm = lq_sta->pers.drv;
	memset(buf, 0, sizeof(buf));
	buf_size = min(count, sizeof(buf) -  1);
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;

	if (sscanf(buf, "%x", &parsed_rate) == 1)
		lq_sta->pers.dbg_fixed_rate = parsed_rate;
	else
		lq_sta->pers.dbg_fixed_rate = 0;

	rs_program_fix_rate(mvm, lq_sta);

	return count;
}

static ssize_t rs_sta_dbgfs_scale_table_read(struct file *file,
			char __user *user_buf, size_t count, loff_t *ppos)
{
	char *buff;
	int desc = 0;
	int i = 0;
	ssize_t ret;

	struct iwl_lq_sta *lq_sta = file->private_data;
	struct iwl_mvm *mvm;
	struct iwl_scale_tbl_info *tbl = &(lq_sta->lq_info[lq_sta->active_tbl]);
	struct rs_rate *rate = &tbl->rate;
	mvm = lq_sta->pers.drv;
	buff = kmalloc(2048, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

	desc += sprintf(buff+desc, "sta_id %d\n", lq_sta->lq.sta_id);
	desc += sprintf(buff+desc, "failed=%d success=%d rate=0%lX\n",
			lq_sta->total_failed, lq_sta->total_success,
			lq_sta->active_legacy_rate);
	desc += sprintf(buff+desc, "fixed rate 0x%X\n",
			lq_sta->pers.dbg_fixed_rate);
	desc += sprintf(buff+desc, "valid_tx_ant %s%s%s\n",
	    (mvm->fw->valid_tx_ant & ANT_A) ? "ANT_A," : "",
	    (mvm->fw->valid_tx_ant & ANT_B) ? "ANT_B," : "",
	    (mvm->fw->valid_tx_ant & ANT_C) ? "ANT_C" : "");
	desc += sprintf(buff+desc, "lq type %s\n",
			(is_legacy(rate)) ? "legacy" :
			is_vht(rate) ? "VHT" : "HT");
	if (!is_legacy(rate)) {
		desc += sprintf(buff+desc, " %s",
		   (is_siso(rate)) ? "SISO" : "MIMO2");
		   desc += sprintf(buff+desc, " %s",
				   (is_ht20(rate)) ? "20MHz" :
				   (is_ht40(rate)) ? "40MHz" :
				   (is_ht80(rate)) ? "80Mhz" : "BAD BW");
		   desc += sprintf(buff+desc, " %s %s %s\n",
				   (rate->sgi) ? "SGI" : "NGI",
				   (rate->ldpc) ? "LDPC" : "BCC",
				   (lq_sta->is_agg) ? "AGG on" : "");
	}
	desc += sprintf(buff+desc, "last tx rate=0x%X\n",
			lq_sta->last_rate_n_flags);
	desc += sprintf(buff+desc,
			"general: flags=0x%X mimo-d=%d s-ant=0x%x d-ant=0x%x\n",
			lq_sta->lq.flags,
			lq_sta->lq.mimo_delim,
			lq_sta->lq.single_stream_ant_msk,
			lq_sta->lq.dual_stream_ant_msk);

	desc += sprintf(buff+desc,
			"agg: time_limit=%d dist_start_th=%d frame_cnt_limit=%d\n",
			le16_to_cpu(lq_sta->lq.agg_time_limit),
			lq_sta->lq.agg_disable_start_th,
			lq_sta->lq.agg_frame_cnt_limit);

	desc += sprintf(buff+desc, "reduced tpc=%d\n", lq_sta->lq.reduced_tpc);
	desc += sprintf(buff+desc,
			"Start idx [0]=0x%x [1]=0x%x [2]=0x%x [3]=0x%x\n",
			lq_sta->lq.initial_rate_index[0],
			lq_sta->lq.initial_rate_index[1],
			lq_sta->lq.initial_rate_index[2],
			lq_sta->lq.initial_rate_index[3]);

	for (i = 0; i < LINK_QUAL_MAX_RETRY_NUM; i++) {
		u32 r = le32_to_cpu(lq_sta->lq.rs_table[i]);

		desc += sprintf(buff+desc, " rate[%d] 0x%X ", i, r);
		desc += rs_pretty_print_rate(buff+desc, r);
	}

	ret = simple_read_from_buffer(user_buf, count, ppos, buff, desc);
	kfree(buff);
	return ret;
}

static const struct file_operations rs_sta_dbgfs_scale_table_ops = {
	.write = rs_sta_dbgfs_scale_table_write,
	.read = rs_sta_dbgfs_scale_table_read,
	.open = simple_open,
	.llseek = default_llseek,
};
static ssize_t rs_sta_dbgfs_stats_table_read(struct file *file,
			char __user *user_buf, size_t count, loff_t *ppos)
{
	char *buff;
	int desc = 0;
	int i, j;
	ssize_t ret;
	struct iwl_scale_tbl_info *tbl;
	struct rs_rate *rate;
	struct iwl_lq_sta *lq_sta = file->private_data;

	buff = kmalloc(1024, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

	for (i = 0; i < LQ_SIZE; i++) {
		tbl = &(lq_sta->lq_info[i]);
		rate = &tbl->rate;
		desc += sprintf(buff+desc,
				"%s type=%d SGI=%d BW=%s DUP=0\n"
				"index=%d\n",
				lq_sta->active_tbl == i ? "*" : "x",
				rate->type,
				rate->sgi,
				is_ht20(rate) ? "20Mhz" :
				is_ht40(rate) ? "40Mhz" :
				is_ht80(rate) ? "80Mhz" : "ERR",
				rate->index);
		for (j = 0; j < IWL_RATE_COUNT; j++) {
			desc += sprintf(buff+desc,
				"counter=%d success=%d %%=%d\n",
				tbl->win[j].counter,
				tbl->win[j].success_counter,
				tbl->win[j].success_ratio);
		}
	}
	ret = simple_read_from_buffer(user_buf, count, ppos, buff, desc);
	kfree(buff);
	return ret;
}

static const struct file_operations rs_sta_dbgfs_stats_table_ops = {
	.read = rs_sta_dbgfs_stats_table_read,
	.open = simple_open,
	.llseek = default_llseek,
};

static ssize_t rs_sta_dbgfs_drv_tx_stats_read(struct file *file,
					      char __user *user_buf,
					      size_t count, loff_t *ppos)
{
	static const char * const column_name[] = {
		[RS_COLUMN_LEGACY_ANT_A] = "LEGACY_ANT_A",
		[RS_COLUMN_LEGACY_ANT_B] = "LEGACY_ANT_B",
		[RS_COLUMN_SISO_ANT_A] = "SISO_ANT_A",
		[RS_COLUMN_SISO_ANT_B] = "SISO_ANT_B",
		[RS_COLUMN_SISO_ANT_A_SGI] = "SISO_ANT_A_SGI",
		[RS_COLUMN_SISO_ANT_B_SGI] = "SISO_ANT_B_SGI",
		[RS_COLUMN_MIMO2] = "MIMO2",
		[RS_COLUMN_MIMO2_SGI] = "MIMO2_SGI",
	};

	static const char * const rate_name[] = {
		[IWL_RATE_1M_INDEX] = "1M",
		[IWL_RATE_2M_INDEX] = "2M",
		[IWL_RATE_5M_INDEX] = "5.5M",
		[IWL_RATE_11M_INDEX] = "11M",
		[IWL_RATE_6M_INDEX] = "6M|MCS0",
		[IWL_RATE_9M_INDEX] = "9M",
		[IWL_RATE_12M_INDEX] = "12M|MCS1",
		[IWL_RATE_18M_INDEX] = "18M|MCS2",
		[IWL_RATE_24M_INDEX] = "24M|MCS3",
		[IWL_RATE_36M_INDEX] = "36M|MCS4",
		[IWL_RATE_48M_INDEX] = "48M|MCS5",
		[IWL_RATE_54M_INDEX] = "54M|MCS6",
		[IWL_RATE_MCS_7_INDEX] = "MCS7",
		[IWL_RATE_MCS_8_INDEX] = "MCS8",
		[IWL_RATE_MCS_9_INDEX] = "MCS9",
	};

	char *buff, *pos, *endpos;
	int col, rate;
	ssize_t ret;
	struct iwl_lq_sta *lq_sta = file->private_data;
	struct rs_rate_stats *stats;
	static const size_t bufsz = 1024;

	buff = kmalloc(bufsz, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

	pos = buff;
	endpos = pos + bufsz;

	pos += scnprintf(pos, endpos - pos, "COLUMN,");
	for (rate = 0; rate < IWL_RATE_COUNT; rate++)
		pos += scnprintf(pos, endpos - pos, "%s,", rate_name[rate]);
	pos += scnprintf(pos, endpos - pos, "\n");

	for (col = 0; col < RS_COLUMN_COUNT; col++) {
		pos += scnprintf(pos, endpos - pos,
				 "%s,", column_name[col]);

		for (rate = 0; rate < IWL_RATE_COUNT; rate++) {
			stats = &(lq_sta->tx_stats[col][rate]);
			pos += scnprintf(pos, endpos - pos,
					 "%llu/%llu,",
					 stats->success,
					 stats->total);
		}
		pos += scnprintf(pos, endpos - pos, "\n");
	}

	ret = simple_read_from_buffer(user_buf, count, ppos, buff, pos - buff);
	kfree(buff);
	return ret;
}

static ssize_t rs_sta_dbgfs_drv_tx_stats_write(struct file *file,
					       const char __user *user_buf,
					       size_t count, loff_t *ppos)
{
	struct iwl_lq_sta *lq_sta = file->private_data;
	memset(lq_sta->tx_stats, 0, sizeof(lq_sta->tx_stats));

	return count;
}

static const struct file_operations rs_sta_dbgfs_drv_tx_stats_ops = {
	.read = rs_sta_dbgfs_drv_tx_stats_read,
	.write = rs_sta_dbgfs_drv_tx_stats_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static void rs_add_debugfs(void *mvm, void *mvm_sta, struct dentry *dir)
{
	struct iwl_lq_sta *lq_sta = mvm_sta;
	lq_sta->pers.rs_sta_dbgfs_scale_table_file =
		debugfs_create_file("rate_scale_table", S_IRUSR | S_IWUSR, dir,
				    lq_sta, &rs_sta_dbgfs_scale_table_ops);
	lq_sta->pers.rs_sta_dbgfs_stats_table_file =
		debugfs_create_file("rate_stats_table", S_IRUSR, dir,
				    lq_sta, &rs_sta_dbgfs_stats_table_ops);
	lq_sta->pers.rs_sta_dbgfs_drv_tx_stats_file =
		debugfs_create_file("drv_tx_stats", S_IRUSR | S_IWUSR, dir,
				    lq_sta, &rs_sta_dbgfs_drv_tx_stats_ops);
	lq_sta->pers.rs_sta_dbgfs_tx_agg_tid_en_file =
		debugfs_create_u8("tx_agg_tid_enable", S_IRUSR | S_IWUSR, dir,
				  &lq_sta->tx_agg_tid_en);
	lq_sta->pers.rs_sta_dbgfs_reduced_txp_file =
		debugfs_create_u8("reduced_tpc", S_IRUSR | S_IWUSR, dir,
				  &lq_sta->pers.dbg_fixed_txp_reduction);
}

static void rs_remove_debugfs(void *mvm, void *mvm_sta)
{
	struct iwl_lq_sta *lq_sta = mvm_sta;
	debugfs_remove(lq_sta->pers.rs_sta_dbgfs_scale_table_file);
	debugfs_remove(lq_sta->pers.rs_sta_dbgfs_stats_table_file);
	debugfs_remove(lq_sta->pers.rs_sta_dbgfs_drv_tx_stats_file);
	debugfs_remove(lq_sta->pers.rs_sta_dbgfs_tx_agg_tid_en_file);
	debugfs_remove(lq_sta->pers.rs_sta_dbgfs_reduced_txp_file);
}
#endif

/*
 * Initialization of rate scaling information is done by driver after
 * the station is added. Since mac80211 calls this function before a
 * station is added we ignore it.
 */
static void rs_rate_init_stub(void *mvm_r,
			      struct ieee80211_supported_band *sband,
			      struct cfg80211_chan_def *chandef,
			      struct ieee80211_sta *sta, void *mvm_sta)
{
}

static const struct rate_control_ops rs_mvm_ops = {
	.name = RS_NAME,
	.tx_status = rs_tx_status,
	.get_rate = rs_get_rate,
	.rate_init = rs_rate_init_stub,
	.alloc = rs_alloc,
	.free = rs_free,
	.alloc_sta = rs_alloc_sta,
	.free_sta = rs_free_sta,
	.rate_update = rs_rate_update,
#ifdef CPTCFG_MAC80211_DEBUGFS
	.add_sta_debugfs = rs_add_debugfs,
	.remove_sta_debugfs = rs_remove_debugfs,
#endif
};

int iwl_mvm_rate_control_register(void)
{
	return ieee80211_rate_control_register(&rs_mvm_ops);
}

void iwl_mvm_rate_control_unregister(void)
{
	ieee80211_rate_control_unregister(&rs_mvm_ops);
}

/**
 * iwl_mvm_tx_protection - Gets LQ command, change it to enable/disable
 * Tx protection, according to this rquest and previous requests,
 * and send the LQ command.
 * @mvmsta: The station
 * @enable: Enable Tx protection?
 */
int iwl_mvm_tx_protection(struct iwl_mvm *mvm, struct iwl_mvm_sta *mvmsta,
			  bool enable)
{
	struct iwl_lq_cmd *lq = &mvmsta->lq_sta.lq;

	lockdep_assert_held(&mvm->mutex);

	if (enable) {
		if (mvmsta->tx_protection == 0)
			lq->flags |= LQ_FLAG_USE_RTS_MSK;
		mvmsta->tx_protection++;
	} else {
		mvmsta->tx_protection--;
		if (mvmsta->tx_protection == 0)
			lq->flags &= ~LQ_FLAG_USE_RTS_MSK;
	}

	return iwl_mvm_send_lq_cmd(mvm, lq, false);
}
