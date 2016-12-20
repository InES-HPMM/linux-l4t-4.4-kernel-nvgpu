/*
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include "gk20a/gk20a.h"
#include "pwrpolicy.h"
#include "include/bios.h"
#include "boardobj/boardobjgrp.h"
#include "boardobj/boardobjgrp_e32.h"
#include "pmuif/gpmuifboardobj.h"
#include "pmuif/gpmuifpmgr.h"
#include "gm206/bios_gm206.h"
#include "gk20a/pmu_gk20a.h"

#define _pwr_policy_limitarboutputget_helper(p_limit_arb) (p_limit_arb)->output
#define _pwr_policy_limitdeltaapply(limit, delta) ((u32)max(((s32)limit) + (delta), 0))

static u32 _pwr_policy_limitarbinputset_helper(struct gk20a *g,
			struct ctrl_pmgr_pwr_policy_limit_arbitration *p_limit_arb,
			u8  client_idx,
			u32 limit_value)
{
	u8 indx;
	bool b_found = false;
	u32 status = 0;
	u32 output = limit_value;

	for (indx = 0; indx< p_limit_arb->num_inputs; indx++) {
		if (p_limit_arb->inputs[indx].pwr_policy_idx == client_idx) {
			p_limit_arb->inputs[indx].limit_value = limit_value;
			b_found = true;
		} else if (p_limit_arb->b_arb_max) {
			output = max(output, p_limit_arb->inputs[indx].limit_value);
		} else {
			output = min(output, p_limit_arb->inputs[indx].limit_value);
		}
	}

	if (!b_found) {
		if (p_limit_arb->num_inputs <
				CTRL_PMGR_PWR_POLICY_MAX_LIMIT_INPUTS) {
			p_limit_arb->inputs[
				p_limit_arb->num_inputs].pwr_policy_idx = client_idx;
			p_limit_arb->inputs[
				p_limit_arb->num_inputs].limit_value = limit_value;
			p_limit_arb->num_inputs++;
		} else {
			gk20a_err(g->dev, "No entries remaining for clientIdx=%d",
				client_idx);
			status = -EINVAL;
		}
	}

	if (!status) {
		p_limit_arb->output = output;
	}

    return status;
}

static u32 _pwr_policy_limitid_translate(struct gk20a *g,
			struct pwr_policy *ppolicy,
			enum pwr_policy_limit_id limit_id,
			struct ctrl_pmgr_pwr_policy_limit_arbitration **p_limit_arb,
			struct ctrl_pmgr_pwr_policy_limit_arbitration **p_limit_arb_sec)
{
	u32 status = 0;

	switch (limit_id) {
		case PWR_POLICY_LIMIT_ID_MIN:
			*p_limit_arb = &ppolicy->limit_arb_min;
			break;

		case PWR_POLICY_LIMIT_ID_RATED:
			*p_limit_arb = &ppolicy->limit_arb_rated;

			if (p_limit_arb_sec != NULL) {
				*p_limit_arb_sec = &ppolicy->limit_arb_curr;
			}
			break;

		case PWR_POLICY_LIMIT_ID_MAX:
			*p_limit_arb = &ppolicy->limit_arb_max;
			break;

		case PWR_POLICY_LIMIT_ID_CURR:
			*p_limit_arb = &ppolicy->limit_arb_curr;
			break;

		case PWR_POLICY_LIMIT_ID_BATT:
			*p_limit_arb = &ppolicy->limit_arb_batt;
			break;

		default:
			gk20a_err(g->dev, "Unsupported limitId=%d",
				limit_id);
			status = -EINVAL;
			break;
	}

	return status;
}

static u32 _pwr_policy_limitarbinputset(struct gk20a *g,
			struct pwr_policy *ppolicy,
			enum pwr_policy_limit_id limit_id,
			u8 client_idx,
			u32 limit)
{
	u32 status = 0;
	struct ctrl_pmgr_pwr_policy_limit_arbitration *p_limit_arb = NULL;
	struct ctrl_pmgr_pwr_policy_limit_arbitration *p_limit_arb_sec = NULL;

	status = _pwr_policy_limitid_translate(g,
			ppolicy,
			limit_id,
			&p_limit_arb,
			&p_limit_arb_sec);
	if (status) {
		goto exit;
	}

	status = _pwr_policy_limitarbinputset_helper(g, p_limit_arb, client_idx, limit);
	if (status) {
		gk20a_err(g->dev,
			"Error setting client limit value: status=0x%08x, limitId=0x%x, clientIdx=0x%x, limit=%d",
			status, limit_id, client_idx, limit);
		goto exit;
	}

	if (NULL != p_limit_arb_sec) {
		status = _pwr_policy_limitarbinputset_helper(g, p_limit_arb_sec,
					CTRL_PMGR_PWR_POLICY_LIMIT_INPUT_CLIENT_IDX_RM,
					_pwr_policy_limitarboutputget_helper(p_limit_arb));
	}

exit:
	return status;
}

static inline void _pwr_policy_limitarbconstruct(
			struct ctrl_pmgr_pwr_policy_limit_arbitration *p_limit_arb,
			bool b_arb_max)
{
	p_limit_arb->num_inputs = 0;
	p_limit_arb->b_arb_max = b_arb_max;
}

static u32 _pwr_policy_limitarboutputget(struct gk20a *g,
			struct pwr_policy *ppolicy,
			enum pwr_policy_limit_id limit_id)
{
	u32 status = 0;
	struct ctrl_pmgr_pwr_policy_limit_arbitration *p_limit_arb = NULL;

	status = _pwr_policy_limitid_translate(g,
				ppolicy,
				limit_id,
				&p_limit_arb,
				NULL);
	if (status) {
		return 0;
	}

	return _pwr_policy_limitarboutputget_helper(p_limit_arb);
}

static u32 _pwr_domains_pmudatainit_hw_threshold(struct gk20a *g,
				struct boardobj *board_obj_ptr,
				struct nv_pmu_boardobj *ppmudata)
{
	struct nv_pmu_pmgr_pwr_policy_hw_threshold *pmu_hw_threshold_data;
	struct pwr_policy_hw_threshold *p_hw_threshold;
	struct pwr_policy *p_pwr_policy;
	struct nv_pmu_pmgr_pwr_policy *pmu_pwr_policy;
	u32 status = 0;

	status = boardobj_pmudatainit_super(g, board_obj_ptr, ppmudata);
	if (status) {
		gk20a_err(dev_from_gk20a(g),
			"error updating pmu boardobjgrp for pwr sensor 0x%x",
			status);
		status = -ENOMEM;
		goto done;
	}

	p_hw_threshold = (struct pwr_policy_hw_threshold *)board_obj_ptr;
	pmu_hw_threshold_data = (struct nv_pmu_pmgr_pwr_policy_hw_threshold *) ppmudata;
	pmu_pwr_policy = (struct nv_pmu_pmgr_pwr_policy *) ppmudata;
	p_pwr_policy = (struct pwr_policy *)&(p_hw_threshold->super.super);

	pmu_pwr_policy->ch_idx = 0;
	pmu_pwr_policy->limit_unit = p_pwr_policy->limit_unit;
	pmu_pwr_policy->num_limit_inputs = p_pwr_policy->num_limit_inputs;

	pmu_pwr_policy->limit_min = _pwr_policy_limitdeltaapply(
			_pwr_policy_limitarboutputget(g, p_pwr_policy,
				PWR_POLICY_LIMIT_ID_MIN),
			p_pwr_policy->limit_delta);

	pmu_pwr_policy->limit_max = _pwr_policy_limitdeltaapply(
			_pwr_policy_limitarboutputget(g, p_pwr_policy,
				PWR_POLICY_LIMIT_ID_MAX),
			p_pwr_policy->limit_delta);

	pmu_pwr_policy->limit_curr = _pwr_policy_limitdeltaapply(
			_pwr_policy_limitarboutputget(g, p_pwr_policy,
				PWR_POLICY_LIMIT_ID_CURR),
			p_pwr_policy->limit_delta);

	memcpy(&pmu_pwr_policy->integral, &p_pwr_policy->integral,
			sizeof(struct ctrl_pmgr_pwr_policy_info_integral));

	pmu_pwr_policy->sample_mult = p_pwr_policy->sample_mult;
	pmu_pwr_policy->filter_type = p_pwr_policy->filter_type;
	pmu_pwr_policy->filter_param = p_pwr_policy->filter_param;

	pmu_hw_threshold_data->threshold_idx = p_hw_threshold->threshold_idx;
	pmu_hw_threshold_data->low_threshold_idx = p_hw_threshold->low_threshold_idx;
	pmu_hw_threshold_data->b_use_low_threshold = p_hw_threshold->b_use_low_threshold;
	pmu_hw_threshold_data->low_threshold_value = p_hw_threshold->low_threshold_value;

	if (BOARDOBJ_GET_TYPE(board_obj_ptr) ==
		CTRL_PMGR_PWR_POLICY_TYPE_SW_THRESHOLD) {
		struct nv_pmu_pmgr_pwr_policy_sw_threshold *pmu_sw_threshold_data;
		struct pwr_policy_sw_threshold *p_sw_threshold;

		p_sw_threshold = (struct pwr_policy_sw_threshold *)board_obj_ptr;
		pmu_sw_threshold_data =
			(struct nv_pmu_pmgr_pwr_policy_sw_threshold *) ppmudata;
		pmu_sw_threshold_data->event_id =
			p_sw_threshold->event_id;
	}
done:
	return status;
}

static struct boardobj *construct_pwr_policy(struct gk20a *g,
			void *pargs, u16 pargs_size, u8 type)
{
	struct boardobj *board_obj_ptr = NULL;
	u32 status;
	struct pwr_policy_hw_threshold *pwrpolicyhwthreshold;
	struct pwr_policy *pwrpolicy;
	struct pwr_policy *pwrpolicyparams = (struct pwr_policy*)pargs;
	struct pwr_policy_hw_threshold *hwthreshold = (struct pwr_policy_hw_threshold*)pargs;

	status = boardobj_construct_super(g, &board_obj_ptr,
		pargs_size, pargs);
	if (status)
		return NULL;

	pwrpolicyhwthreshold = (struct pwr_policy_hw_threshold*)board_obj_ptr;
	pwrpolicy = (struct pwr_policy *)board_obj_ptr;

	/* Set Super class interfaces */
	board_obj_ptr->pmudatainit = _pwr_domains_pmudatainit_hw_threshold;

	pwrpolicy->ch_idx = pwrpolicyparams->ch_idx;
	pwrpolicy->num_limit_inputs = 0;
	pwrpolicy->limit_unit = pwrpolicyparams->limit_unit;
	pwrpolicy->filter_type = (enum ctrl_pmgr_pwr_policy_filter_type)(pwrpolicyparams->filter_type);
	pwrpolicy->sample_mult = pwrpolicyparams->sample_mult;
	switch (pwrpolicy->filter_type)
	{
		case CTRL_PMGR_PWR_POLICY_FILTER_TYPE_NONE:
			break;

		case CTRL_PMGR_PWR_POLICY_FILTER_TYPE_BLOCK:
			pwrpolicy->filter_param.block.block_size =
				pwrpolicyparams->filter_param.block.block_size;
			break;

		case CTRL_PMGR_PWR_POLICY_FILTER_TYPE_MOVING_AVERAGE:
			pwrpolicy->filter_param.moving_avg.window_size =
				pwrpolicyparams->filter_param.moving_avg.window_size;
			break;

		case CTRL_PMGR_PWR_POLICY_FILTER_TYPE_IIR:
			pwrpolicy->filter_param.iir.divisor = pwrpolicyparams->filter_param.iir.divisor;
			break;

		default:
		gk20a_err(g->dev,
			"Error: unrecognized Power Policy filter type: %d.\n",
			pwrpolicy->filter_type);
	}

	_pwr_policy_limitarbconstruct(&pwrpolicy->limit_arb_curr, false);

	pwrpolicy->limit_delta = 0;

	_pwr_policy_limitarbconstruct(&pwrpolicy->limit_arb_min, true);
	status = _pwr_policy_limitarbinputset(g,
			pwrpolicy,
			PWR_POLICY_LIMIT_ID_MIN,
			CTRL_PMGR_PWR_POLICY_LIMIT_INPUT_CLIENT_IDX_RM,
			pwrpolicyparams->limit_min);

	_pwr_policy_limitarbconstruct(&pwrpolicy->limit_arb_max, false);
	status = _pwr_policy_limitarbinputset(g,
			pwrpolicy,
			PWR_POLICY_LIMIT_ID_MAX,
			CTRL_PMGR_PWR_POLICY_LIMIT_INPUT_CLIENT_IDX_RM,
			pwrpolicyparams->limit_max);

	_pwr_policy_limitarbconstruct(&pwrpolicy->limit_arb_rated, false);
	status = _pwr_policy_limitarbinputset(g,
			pwrpolicy,
			PWR_POLICY_LIMIT_ID_RATED,
			CTRL_PMGR_PWR_POLICY_LIMIT_INPUT_CLIENT_IDX_RM,
			pwrpolicyparams->limit_rated);

	_pwr_policy_limitarbconstruct(&pwrpolicy->limit_arb_batt, false);
	status = _pwr_policy_limitarbinputset(g,
			pwrpolicy,
			PWR_POLICY_LIMIT_ID_BATT,
			CTRL_PMGR_PWR_POLICY_LIMIT_INPUT_CLIENT_IDX_RM,
			((pwrpolicyparams->limit_batt != 0) ?
				pwrpolicyparams->limit_batt:
				CTRL_PMGR_PWR_POLICY_LIMIT_MAX));

	memcpy(&pwrpolicy->integral, &pwrpolicyparams->integral,
			sizeof(struct ctrl_pmgr_pwr_policy_info_integral));

	pwrpolicyhwthreshold->threshold_idx = hwthreshold->threshold_idx;
	pwrpolicyhwthreshold->b_use_low_threshold = hwthreshold->b_use_low_threshold;
	pwrpolicyhwthreshold->low_threshold_idx = hwthreshold->low_threshold_idx;
	pwrpolicyhwthreshold->low_threshold_value = hwthreshold->low_threshold_value;

	if (type == CTRL_PMGR_PWR_POLICY_TYPE_SW_THRESHOLD) {
		struct pwr_policy_sw_threshold *pwrpolicyswthreshold;
		struct pwr_policy_sw_threshold *swthreshold =
			(struct pwr_policy_sw_threshold*)pargs;

		pwrpolicyswthreshold = (struct pwr_policy_sw_threshold*)board_obj_ptr;
		pwrpolicyswthreshold->event_id = swthreshold->event_id;
	}

	gk20a_dbg_info(" Done");

	return board_obj_ptr;
}

static u32 _pwr_policy_construct_WAR_policy(struct gk20a *g,
			struct pmgr_pwr_policy *ppwrpolicyobjs,
			union pwr_policy_data_union *ppwrpolicydata,
			u16 pwr_policy_size,
			u32 hw_threshold_policy_index,
			u32 obj_index)
{
	u32 status = 0;
	struct boardobj *boardobj;

	if (!(hw_threshold_policy_index & 0x1)) {
		/* CRIT policy */
		ppwrpolicydata->pwrpolicy.limit_min = 1000;
		ppwrpolicydata->pwrpolicy.limit_rated = 20000;
		ppwrpolicydata->pwrpolicy.limit_max = 20000;
		ppwrpolicydata->hw_threshold.threshold_idx = 0;
	} else {
		/* WARN policy */
		ppwrpolicydata->pwrpolicy.limit_min = 1000;
		ppwrpolicydata->pwrpolicy.limit_rated = 11600;
		ppwrpolicydata->pwrpolicy.limit_max = 11600;
		ppwrpolicydata->hw_threshold.threshold_idx = 1;
	}

	boardobj = construct_pwr_policy(g, ppwrpolicydata,
				pwr_policy_size, ppwrpolicydata->boardobj.type);

	if (!boardobj) {
		gk20a_err(dev_from_gk20a(g),
			"unable to create pwr policy for type %d", ppwrpolicydata->boardobj.type);
		status = -EINVAL;
		goto done;
	}

	status = boardobjgrp_objinsert(&ppwrpolicyobjs->pwr_policies.super,
			boardobj, obj_index);

	if (status) {
		gk20a_err(dev_from_gk20a(g),
			"unable to insert pwr policy boardobj for %d", obj_index);
		status = -EINVAL;
		goto done;
	}
done:
	return status;
}

static u32 _pwr_policy_construct_WAR_SW_Threshold_policy(struct gk20a *g,
			struct pmgr_pwr_policy *ppwrpolicyobjs,
			union pwr_policy_data_union *ppwrpolicydata,
			u16 pwr_policy_size,
			u32 obj_index)
{
	u32 status = 0;
	struct boardobj *boardobj;

	/* WARN policy */
	ppwrpolicydata->pwrpolicy.limit_unit = 0;
	ppwrpolicydata->pwrpolicy.limit_min = 10000;
	ppwrpolicydata->pwrpolicy.limit_rated = 100000;
	ppwrpolicydata->pwrpolicy.limit_max = 100000;
	ppwrpolicydata->sw_threshold.threshold_idx = 1;
	ppwrpolicydata->pwrpolicy.filter_type =
			CTRL_PMGR_PWR_POLICY_FILTER_TYPE_MOVING_AVERAGE;
	ppwrpolicydata->pwrpolicy.sample_mult  = 5;

	/* Filled the entry.filterParam value in the filterParam */
	ppwrpolicydata->pwrpolicy.filter_param.moving_avg.window_size = 10;

	ppwrpolicydata->sw_threshold.event_id = 0x01;

	ppwrpolicydata->boardobj.type = CTRL_PMGR_PWR_POLICY_TYPE_SW_THRESHOLD;

	boardobj = construct_pwr_policy(g, ppwrpolicydata,
				pwr_policy_size, ppwrpolicydata->boardobj.type);

	if (!boardobj) {
		gk20a_err(dev_from_gk20a(g),
			"unable to create pwr policy for type %d", ppwrpolicydata->boardobj.type);
		status = -EINVAL;
		goto done;
	}

	status = boardobjgrp_objinsert(&ppwrpolicyobjs->pwr_policies.super,
			boardobj, obj_index);

	if (status) {
		gk20a_err(dev_from_gk20a(g),
			"unable to insert pwr policy boardobj for %d", obj_index);
		status = -EINVAL;
		goto done;
	}
done:
	return status;
}

static u32 devinit_get_pwr_policy_table(struct gk20a *g,
			struct pmgr_pwr_policy *ppwrpolicyobjs)
{
	u32 status = 0;
	u8 *pwr_policy_table_ptr = NULL;
	u8 *curr_pwr_policy_table_ptr = NULL;
	struct boardobj *boardobj;
	struct pwr_policy_3x_header_struct pwr_policy_table_header = { 0 };
	struct pwr_policy_3x_entry_struct pwr_policy_table_entry = { 0 };
	u32 index;
	u32 obj_index = 0;
	u16 pwr_policy_size;
	bool integral_control = false;
	u32 hw_threshold_policy_index = 0;
	u32 sw_threshold_policy_index = 0;
	union pwr_policy_data_union pwr_policy_data;

	gk20a_dbg_info("");

	if (g->ops.bios.get_perf_table_ptrs != NULL) {
		pwr_policy_table_ptr = (u8 *)g->ops.bios.get_perf_table_ptrs(g,
				g->bios.perf_token, POWER_CAPPING_TABLE);
		if (pwr_policy_table_ptr == NULL) {
			status = -EINVAL;
			goto done;
		}
	}

	memcpy(&pwr_policy_table_header.version,
		(pwr_policy_table_ptr),
		14);

	memcpy(&pwr_policy_table_header.d2_limit,
		(pwr_policy_table_ptr + 14),
		(VBIOS_POWER_POLICY_3X_ENTRY_SIZE_2E - 14));

	if (pwr_policy_table_header.version !=
			VBIOS_POWER_POLICY_VERSION_3X) {
		status = -EINVAL;
		goto done;
	}

	if (pwr_policy_table_header.header_size <
			VBIOS_POWER_POLICY_3X_HEADER_SIZE_25) {
		status = -EINVAL;
		goto done;
	}

	if (pwr_policy_table_header.table_entry_size !=
			VBIOS_POWER_POLICY_3X_ENTRY_SIZE_2E) {
		status = -EINVAL;
		goto done;
	}

	curr_pwr_policy_table_ptr = (pwr_policy_table_ptr +
		VBIOS_POWER_POLICY_3X_HEADER_SIZE_25);

	for (index = 0; index < pwr_policy_table_header.num_table_entries;
		index++) {
		u8 class_type;

		curr_pwr_policy_table_ptr += (pwr_policy_table_header.table_entry_size * index);

		pwr_policy_table_entry.flags0 = *curr_pwr_policy_table_ptr;
		pwr_policy_table_entry.ch_idx = *(curr_pwr_policy_table_ptr + 1);

		memcpy(&pwr_policy_table_entry.limit_min,
			(curr_pwr_policy_table_ptr + 2),
			35);

		memcpy(&pwr_policy_table_entry.ratio_min,
			(curr_pwr_policy_table_ptr + 2 + 35),
			4);

		pwr_policy_table_entry.sample_mult =
				*(curr_pwr_policy_table_ptr + 2 + 35 + 4);

		memcpy(&pwr_policy_table_entry.filter_param,
			(curr_pwr_policy_table_ptr + 2 + 35 + 4 + 1),
			4);

		class_type = (u8)BIOS_GET_FIELD(
			pwr_policy_table_entry.flags0,
			NV_VBIOS_POWER_POLICY_3X_ENTRY_FLAGS0_CLASS);

		if (class_type == NV_VBIOS_POWER_POLICY_3X_ENTRY_FLAGS0_CLASS_HW_THRESHOLD) {
			ppwrpolicyobjs->version = CTRL_PMGR_PWR_POLICY_TABLE_VERSION_3X;
			ppwrpolicyobjs->base_sample_period = (u16)
					pwr_policy_table_header.base_sample_period;
			ppwrpolicyobjs->min_client_sample_period = (u16)
					pwr_policy_table_header.min_client_sample_period;
			ppwrpolicyobjs->low_sampling_mult =
					pwr_policy_table_header.low_sampling_mult;

			ppwrpolicyobjs->policy_idxs[1] =
				(u8)pwr_policy_table_header.tgp_policy_idx;
			ppwrpolicyobjs->policy_idxs[0] =
				(u8)pwr_policy_table_header.rtp_policy_idx;
			ppwrpolicyobjs->policy_idxs[2] =
				pwr_policy_table_header.mxm_policy_idx;
			ppwrpolicyobjs->policy_idxs[3] =
				pwr_policy_table_header.dnotifier_policy_idx;
			ppwrpolicyobjs->ext_limits[0].limit =
				pwr_policy_table_header.d2_limit;
			ppwrpolicyobjs->ext_limits[1].limit =
				pwr_policy_table_header.d3_limit;
			ppwrpolicyobjs->ext_limits[2].limit =
				pwr_policy_table_header.d4_limit;
			ppwrpolicyobjs->ext_limits[3].limit =
				pwr_policy_table_header.d5_limit;
			ppwrpolicyobjs->policy_idxs[4] =
				pwr_policy_table_header.pwr_tgt_policy_idx;
			ppwrpolicyobjs->policy_idxs[5] =
				pwr_policy_table_header.pwr_tgt_floor_policy_idx;
			ppwrpolicyobjs->policy_idxs[6] =
				pwr_policy_table_header.sm_bus_policy_idx;

			integral_control = (bool)BIOS_GET_FIELD(
				pwr_policy_table_entry.flags1,
				NV_VBIOS_POWER_POLICY_3X_ENTRY_FLAGS1_INTEGRAL_CONTROL);

			if (integral_control == 0x01) {
				pwr_policy_data.pwrpolicy.integral.past_sample_count = (u8)
						pwr_policy_table_entry.past_length;
				pwr_policy_data.pwrpolicy.integral.next_sample_count = (u8)
						pwr_policy_table_entry.next_length;
				pwr_policy_data.pwrpolicy.integral.ratio_limit_max = (u16)
						pwr_policy_table_entry.ratio_max;
				pwr_policy_data.pwrpolicy.integral.ratio_limit_min = (u16)
						pwr_policy_table_entry.ratio_min;
			} else {
				memset(&(pwr_policy_data.pwrpolicy.integral), 0x0,
					sizeof(struct ctrl_pmgr_pwr_policy_info_integral));
			}
			pwr_policy_data.hw_threshold.threshold_idx = (u8)
				BIOS_GET_FIELD(
					pwr_policy_table_entry.param0,
					NV_VBIOS_POWER_POLICY_3X_ENTRY_PARAM0_HW_THRESHOLD_THRES_IDX);

			pwr_policy_data.hw_threshold.b_use_low_threshold =
				BIOS_GET_FIELD(
					pwr_policy_table_entry.param0,
					NV_VBIOS_POWER_POLICY_3X_ENTRY_PARAM0_HW_THRESHOLD_LOW_THRESHOLD_USE);

			if (pwr_policy_data.hw_threshold.b_use_low_threshold) {
				pwr_policy_data.hw_threshold.low_threshold_idx = (u8)
					BIOS_GET_FIELD(
						pwr_policy_table_entry.param0,
						NV_VBIOS_POWER_POLICY_3X_ENTRY_PARAM0_HW_THRESHOLD_LOW_THRESHOLD_IDX);

				pwr_policy_data.hw_threshold.low_threshold_value = (u16)
					BIOS_GET_FIELD(
						pwr_policy_table_entry.param1,
						NV_VBIOS_POWER_POLICY_3X_ENTRY_PARAM1_HW_THRESHOLD_LOW_THRESHOLD_VAL);
			}

			pwr_policy_size = sizeof(struct pwr_policy_hw_threshold);
		} else
			continue;

		/* Initialize data for the parent class */
		pwr_policy_data.boardobj.type = CTRL_PMGR_PWR_POLICY_TYPE_HW_THRESHOLD;
		pwr_policy_data.pwrpolicy.ch_idx = (u8)pwr_policy_table_entry.ch_idx;
		pwr_policy_data.pwrpolicy.limit_unit = (u8)
				BIOS_GET_FIELD(
					pwr_policy_table_entry.flags0,
					NV_VBIOS_POWER_POLICY_3X_ENTRY_FLAGS0_LIMIT_UNIT);
		pwr_policy_data.pwrpolicy.filter_type = (u8)
				BIOS_GET_FIELD(
					pwr_policy_table_entry.flags1,
					NV_VBIOS_POWER_POLICY_3X_ENTRY_FLAGS1_FILTER_TYPE);
		pwr_policy_data.pwrpolicy.limit_min = pwr_policy_table_entry.limit_min;
		pwr_policy_data.pwrpolicy.limit_rated = pwr_policy_table_entry.limit_rated;
		pwr_policy_data.pwrpolicy.limit_max = pwr_policy_table_entry.limit_max;
		pwr_policy_data.pwrpolicy.limit_batt = pwr_policy_table_entry.limit_batt;

		pwr_policy_data.pwrpolicy.sample_mult  = (u8)pwr_policy_table_entry.sample_mult;

		/* Filled the entry.filterParam value in the filterParam */
		pwr_policy_data.pwrpolicy.filter_param.block.block_size = 0;
		pwr_policy_data.pwrpolicy.filter_param.moving_avg.window_size = 0;
		pwr_policy_data.pwrpolicy.filter_param.iir.divisor = 0;

		hw_threshold_policy_index |=
			BIT(pwr_policy_data.hw_threshold.threshold_idx);

		boardobj = construct_pwr_policy(g, &pwr_policy_data,
					pwr_policy_size, pwr_policy_data.boardobj.type);

		if (!boardobj) {
			gk20a_err(dev_from_gk20a(g),
				"unable to create pwr policy for %d type %d", index, pwr_policy_data.boardobj.type);
			status = -EINVAL;
			goto done;
		}

		status = boardobjgrp_objinsert(&ppwrpolicyobjs->pwr_policies.super,
				boardobj, obj_index);

		if (status) {
			gk20a_err(dev_from_gk20a(g),
				"unable to insert pwr policy boardobj for %d", index);
			status = -EINVAL;
			goto done;
		}

		++obj_index;
	}

	if (hw_threshold_policy_index &&
		(hw_threshold_policy_index < 0x3)) {
		status = _pwr_policy_construct_WAR_policy(g,
					ppwrpolicyobjs,
					&pwr_policy_data,
					pwr_policy_size,
					hw_threshold_policy_index,
					obj_index);
		if (status) {
			gk20a_err(dev_from_gk20a(g),
				"unable to construct_WAR_policy");
			status = -EINVAL;
			goto done;
		}
		++obj_index;
	}

	if (!sw_threshold_policy_index) {
		status = _pwr_policy_construct_WAR_SW_Threshold_policy(g,
					ppwrpolicyobjs,
					&pwr_policy_data,
					sizeof(struct pwr_policy_sw_threshold),
					obj_index);
		if (status) {
			gk20a_err(dev_from_gk20a(g),
				"unable to construct_WAR_policy");
			status = -EINVAL;
			goto done;
		}
		++obj_index;
	}

done:
	gk20a_dbg_info(" done status %x", status);
	return status;
}

u32 pmgr_policy_sw_setup(struct gk20a *g)
{
	u32 status;
	struct boardobjgrp *pboardobjgrp = NULL;
	struct pwr_policy *ppolicy;
	struct pmgr_pwr_policy *ppwrpolicyobjs;
	u8 indx = 0;

	/* Construct the Super Class and override the Interfaces */
	status = boardobjgrpconstruct_e32(
		&g->pmgr_pmu.pmgr_policyobjs.pwr_policies);
	if (status) {
		gk20a_err(dev_from_gk20a(g),
			"error creating boardobjgrp for pmgr policy, status - 0x%x",
			status);
		goto done;
	}

	status = boardobjgrpconstruct_e32(
		&g->pmgr_pmu.pmgr_policyobjs.pwr_policy_rels);
	if (status) {
		gk20a_err(dev_from_gk20a(g),
			"error creating boardobjgrp for pmgr policy rels, status - 0x%x",
			status);
		goto done;
	}

	status = boardobjgrpconstruct_e32(
		&g->pmgr_pmu.pmgr_policyobjs.pwr_violations);
	if (status) {
		gk20a_err(dev_from_gk20a(g),
			"error creating boardobjgrp for pmgr violations, status - 0x%x",
			status);
		goto done;
	}

	memset(g->pmgr_pmu.pmgr_policyobjs.policy_idxs, CTRL_PMGR_PWR_POLICY_INDEX_INVALID,
				sizeof(u8) * CTRL_PMGR_PWR_POLICY_IDX_NUM_INDEXES);

	/* Initialize external power limit policy indexes to _INVALID/0xFF */
	for (indx = 0; indx < PWR_POLICY_EXT_POWER_STATE_ID_COUNT; indx++) {
		g->pmgr_pmu.pmgr_policyobjs.ext_limits[indx].policy_table_idx =
			CTRL_PMGR_PWR_POLICY_INDEX_INVALID;
	}

	/* Initialize external power state to _D1 */
	g->pmgr_pmu.pmgr_policyobjs.ext_power_state = 0xFFFFFFFF;

	ppwrpolicyobjs = &(g->pmgr_pmu.pmgr_policyobjs);
	pboardobjgrp = &(g->pmgr_pmu.pmgr_policyobjs.pwr_policies.super);

	status = devinit_get_pwr_policy_table(g, ppwrpolicyobjs);
	if (status)
		goto done;

	g->pmgr_pmu.pmgr_policyobjs.b_enabled = true;

	BOARDOBJGRP_FOR_EACH(pboardobjgrp, struct pwr_policy *, ppolicy, indx) {
		PMGR_PWR_POLICY_INCREMENT_LIMIT_INPUT_COUNT(ppolicy);
	}

	g->pmgr_pmu.pmgr_policyobjs.global_ceiling.values[0] =
				0xFF;

	g->pmgr_pmu.pmgr_policyobjs.client_work_item.b_pending = false;

done:
	gk20a_dbg_info(" done status %x", status);
	return status;
}