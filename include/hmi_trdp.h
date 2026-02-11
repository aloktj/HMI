#ifndef HMI_TRDP_H
#define HMI_TRDP_H

#include "trdp_if_light.h"

#define HMI_PD_SUB_COMID            2001u
#define HMI_PD_PUB_COMID            2002u
#define HMI_DOOR_CMD_PD_COMID_BASE  2101u
#define HMI_DOOR_COUNT              8u
#define HMI_MD_RX_COMID             2201u

#define HMI_PD_PAYLOAD_SIZE         64u
#define HMI_DOOR_CMD_PAYLOAD_SIZE   8u

#define HMI_PD_CYCLE_US             100000u
#define HMI_PD_TIMEOUT_US           300000u
#define HMI_MAIN_LOOP_SLEEP_US      10000u

#endif /* HMI_TRDP_H */
