#ifndef HMI_TRDP_H
#define HMI_TRDP_H

#include "trdp_if_light.h"

#define HMI_PD_SUB_COMID            2001u
#define HMI_PD_PUB_COMID            2002u
#define HMI_MD_TX_COMID             2101u
#define HMI_MD_RX_COMID             2102u

#define HMI_DOOR_COUNT              4u
#define HMI_PD_PAYLOAD_SIZE         64u
#define HMI_MD_PAYLOAD_SIZE         16u

#define HMI_PD_CYCLE_US             100000u
#define HMI_PD_TIMEOUT_US           300000u
#define HMI_MAIN_LOOP_SLEEP_US      10000u

#endif /* HMI_TRDP_H */
