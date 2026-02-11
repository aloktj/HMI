#ifndef HMI_TRDP_H
#define HMI_TRDP_H

/*
 * HMI TRDP Configuration Header
 *
 * Aligned with CAN ICD: "Doors CAN Interface Control Document"
 *   - Door_Status (CAN 0x301..0x308): 8 bytes per door
 *   - Door_Command (CAN 0x401..0x408): 8 bytes per door
 *
 * Gateway aggregates 8 doors into single 64-byte TRDP PD telegrams.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "trdp_if_light.h"
#include "vos_sock.h"
#include "vos_thread.h"
#include "vos_utils.h"

#ifdef __cplusplus
}
#endif

/* ---------- ComId assignments ---------- */
#define HMI_PD_DOOR_STATUS_COMID    2001u   /* Gateway -> HMI: aggregated door status */
#define HMI_PD_HMI_STATUS_COMID     2002u   /* HMI -> Gateway: HMI heartbeat          */
#define HMI_PD_DOOR_CMD_COMID       2010u   /* HMI -> Gateway: aggregated door command */
#define HMI_MD_RX_COMID             2201u   /* Gateway -> HMI: message data (optional) */

/* ---------- Door configuration ---------- */
#define HMI_DOOR_COUNT              8u

/* ---------- Timing ---------- */
#define HMI_PD_CYCLE_US             100000u /* 100 ms - matches CAN ICD period        */
#define HMI_PD_TIMEOUT_US           300000u /* 300 ms - matches CAN ICD cmd timeout    */
#define HMI_TRDP_LOOP_SLEEP_US     10000u  /* 10 ms TRDP processing tick              */
#define HMI_WEB_PORT                8080u   /* Crow web server port                    */

/*
 * ---------- Payload structures ----------
 *
 * Each 8-byte block maps 1:1 to a CAN frame body.
 * Aggregated PD = 8 doors x 8 bytes = 64 bytes.
 */

#define HMI_DOOR_ENTRY_SIZE         8u
#define HMI_AGGREGATED_PD_SIZE      (HMI_DOOR_COUNT * HMI_DOOR_ENTRY_SIZE)  /* 64 */
#define HMI_HMI_STATUS_PD_SIZE      8u

/*
 * Door Status entry (Door -> Gateway -> HMI), per CAN ICD Section 4.
 * CAN ID: 0x300 + DoorID
 */
typedef struct __attribute__((packed))
{
    uint8_t door_state;       /* B0: 0=OPEN, 1=CLOSED                      */
    uint8_t obstruction;      /* B1: 0=NO, 1=YES                           */
    uint8_t last_cmd;         /* B2: 0=NONE, 1=OPEN, 2=CLOSE               */
    uint8_t close_blocked;    /* B3: 0=NO, 1=CLOSE blocked due to obstruction */
    uint8_t status_counter;   /* B4: incrementing counter (rollover OK)     */
    uint8_t reserved5;        /* B5: always 0                               */
    uint8_t reserved6;        /* B6: always 0                               */
    uint8_t reserved7;        /* B7: always 0                               */
} DoorStatusEntry_T;

/*
 * Door Command entry (HMI -> Gateway -> Door), per CAN ICD Section 5.
 * CAN ID: 0x400 + DoorID
 */
typedef struct __attribute__((packed))
{
    uint8_t cmd;              /* B0: 0=NONE, 1=OPEN, 2=CLOSE               */
    uint8_t alive_counter;    /* B1: incremented when HMI intent changes    */
    uint8_t reserved2;        /* B2: always 0                               */
    uint8_t reserved3;        /* B3: always 0                               */
    uint8_t reserved4;        /* B4: always 0                               */
    uint8_t reserved5;        /* B5: always 0                               */
    uint8_t reserved6;        /* B6: always 0                               */
    uint8_t reserved7;        /* B7: always 0                               */
} DoorCommandEntry_T;

/*
 * Aggregated payloads (64 bytes each).
 * Gateway packs/unpacks these from individual CAN frames.
 */
typedef struct __attribute__((packed))
{
    DoorStatusEntry_T doors[HMI_DOOR_COUNT];
} AggregatedDoorStatus_T;

typedef struct __attribute__((packed))
{
    DoorCommandEntry_T doors[HMI_DOOR_COUNT];
} AggregatedDoorCommand_T;

/* ---------- CAN ICD command values ---------- */
#define DOOR_CMD_NONE               0u
#define DOOR_CMD_OPEN               1u
#define DOOR_CMD_CLOSE              2u

/* ---------- CAN ICD state values ---------- */
#define DOOR_STATE_OPEN             0u
#define DOOR_STATE_CLOSED           1u

#endif /* HMI_TRDP_H */
