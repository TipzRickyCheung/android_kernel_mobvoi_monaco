/* Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
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

#ifndef CAM_BPS_HW_INTF_H
#define CAM_BPS_HW_INTF_H

#include <media/cam_defs.h>
#include <media/cam_icp.h>
#include "cam_hw_mgr_intf.h"
#include "cam_icp_hw_intf.h"

/* BPS register */
#define BPS_TOP_RST_CMD              0x1008
#define BPS_CDM_RST_CMD              0x10
#define BPS_CDM_IRQ_STATUS           0x44
#define BPS_TOP_IRQ_STATUS           0x100C

/* BPS CDM/TOP status register */
#define BPS_RST_DONE_IRQ_STATUS_BIT  0x1

enum cam_icp_bps_cmd_type {
	CAM_ICP_BPS_CMD_FW_DOWNLOAD,
	CAM_ICP_BPS_CMD_POWER_COLLAPSE,
	CAM_ICP_BPS_CMD_POWER_RESUME,
	CAM_ICP_BPS_CMD_SET_FW_BUF,
	CAM_ICP_BPS_CMD_VOTE_CPAS,
	CAM_ICP_BPS_CMD_CPAS_START,
	CAM_ICP_BPS_CMD_CPAS_STOP,
	CAM_ICP_BPS_CMD_UPDATE_CLK,
	CAM_ICP_BPS_CMD_DISABLE_CLK,
	CAM_ICP_BPS_CMD_RESET,
	CAM_ICP_BPS_CMD_MAX,
};

#endif /* CAM_BPS_HW_INTF_H */
