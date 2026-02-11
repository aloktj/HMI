#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/time.h>

#include "trdp_if_light.h"
#include "vos_sock.h"
#include "vos_thread.h"
#include "vos_utils.h"

#include "hmi_trdp.h"

typedef struct
{
    TRDP_SUB_T handle;
    TRDP_IP_ADDR_T destination;
    const char *name;
} HMI_SUBSCRIPTION_T;

typedef struct
{
    TRDP_PUB_T handle;
    UINT32 comId;
} HMI_DOOR_CMD_PUBLISHER_T;

static void usage(const char *app)
{
    printf("Usage: %s [runtime_s] [own_ip] [gateway_ip] [mc_a] [mc_b]\n", app);
    printf("Defaults: 0 192.168.56.2 192.168.56.1 239.192.0.1 239.192.0.2\n");
}

static void log_cb(void *pRefCon,
                   TRDP_LOG_T category,
                   const CHAR8 *pTime,
                   const CHAR8 *pFile,
                   UINT16 lineNumber,
                   const CHAR8 *pMsgStr)
{
    (void)pRefCon;
    const char *cat[] = {"ERROR", "WARN", "INFO", "DEBUG", "USER"};
    const char *fileName = strrchr(pFile, VOS_DIR_SEP);
    printf("[%s] %s %s:%u %s", cat[category], pTime, fileName ? fileName + 1 : pFile, lineNumber, pMsgStr);
}

static void md_cb(void *pRefCon,
                  TRDP_APP_SESSION_T appHandle,
                  const TRDP_MD_INFO_T *pMsg,
                  UINT8 *pData,
                  UINT32 dataSize)
{
    (void)pRefCon;
    (void)appHandle;
    printf("MD rx: comId=%u msgType=0x%04x from=%s size=%u\n",
           pMsg->comId,
           pMsg->msgType,
           vos_ipDotted(pMsg->srcIpAddr),
           dataSize);

    if (pData != NULL && dataSize > 0u)
    {
        UINT32 i;
        printf("MD payload:");
        for (i = 0u; i < dataSize; ++i)
        {
            printf(" %02X", pData[i]);
        }
        printf("\n");
    }
}

static TRDP_ERR_T add_pd_subscription(TRDP_APP_SESSION_T appHandle,
                                      HMI_SUBSCRIPTION_T *sub,
                                      UINT32 gatewayIp,
                                      UINT32 comId)
{
    return tlp_subscribe(appHandle,
                         &sub->handle,
                         NULL,
                         NULL,
                         0u,
                         comId,
                         0u,
                         0u,
                         gatewayIp,
                         gatewayIp,
                         sub->destination,
                         TRDP_FLAGS_NONE,
                         HMI_PD_TIMEOUT_US,
                         TRDP_TO_SET_TO_ZERO);
}

static void fill_demo_payload(UINT8 *buffer, UINT32 size, UINT8 seed)
{
    UINT32 i;
    for (i = 0u; i < size; ++i)
    {
        buffer[i] = (UINT8)(seed + i);
    }
}

static void print_door_command_prompt(void)
{
    printf("Door command input: <door_index 0-%u> <cmd 0=open 1=close>, or q to quit input\n", HMI_DOOR_COUNT - 1u);
}

static int read_door_command(UINT32 *doorIndex, UINT8 *cmd)
{
    fd_set readFds;
    struct timeval timeout = {0, 0};
    int selectResult;
    char line[128];
    unsigned int parsedDoor;
    unsigned int parsedCmd;

    FD_ZERO(&readFds);
    FD_SET(0, &readFds);
    selectResult = select(1, &readFds, NULL, NULL, &timeout);
    if (selectResult <= 0)
    {
        return 0;
    }

    if (fgets(line, sizeof(line), stdin) == NULL)
    {
        return 0;
    }

    if (line[0] == 'q' || line[0] == 'Q')
    {
        return -1;
    }

    if (sscanf(line, "%u %u", &parsedDoor, &parsedCmd) != 2)
    {
        printf("Invalid input. Example: 3 1 (door 3 close)\n");
        print_door_command_prompt();
        return 0;
    }

    if (parsedDoor >= HMI_DOOR_COUNT || (parsedCmd != 0u && parsedCmd != 1u))
    {
        printf("Out of range. door_index: 0-%u, cmd: 0(open)/1(close)\n", HMI_DOOR_COUNT - 1u);
        print_door_command_prompt();
        return 0;
    }

    *doorIndex = (UINT32)parsedDoor;
    *cmd = (UINT8)parsedCmd;
    return 1;
}

static void build_door_cmd_payload(UINT8 *payload, UINT8 cmd, UINT8 aliveCounter)
{
    memset(payload, 0, HMI_DOOR_CMD_PAYLOAD_SIZE);
    payload[0] = cmd;
    payload[1] = aliveCounter;
}

int main(int argc, char **argv)
{
    UINT32 runtimeSeconds = 0u;
    UINT32 ownIp = vos_dottedIP("192.168.56.2");
    UINT32 gatewayIp = vos_dottedIP("192.168.56.1");
    UINT32 multicastA = vos_dottedIP("239.192.0.1");
    UINT32 multicastB = vos_dottedIP("239.192.0.2");

    if (argc > 1)
    {
        runtimeSeconds = (UINT32)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2)
    {
        ownIp = vos_dottedIP(argv[2]);
    }
    if (argc > 3)
    {
        gatewayIp = vos_dottedIP(argv[3]);
    }
    if (argc > 4)
    {
        multicastA = vos_dottedIP(argv[4]);
    }
    if (argc > 5)
    {
        multicastB = vos_dottedIP(argv[5]);
    }
    if (argc > 6)
    {
        usage(argv[0]);
        return 1;
    }

    TRDP_MEM_CONFIG_T memConfig = {NULL, 512000u, {0}};
    TRDP_PROCESS_CONFIG_T processConfig = {
        "HMI",
        "HMI TRDP endpoint",
        "",
        TRDP_PROCESS_DEFAULT_CYCLE_TIME,
        0u,
        TRDP_OPTION_TRAFFIC_SHAPING,
        0u};
    TRDP_PD_CONFIG_T pdConfig = {
        NULL,
        NULL,
        TRDP_PD_DEFAULT_SEND_PARAM,
        TRDP_FLAGS_NONE,
        HMI_PD_TIMEOUT_US,
        TRDP_TO_SET_TO_ZERO,
        0u};
    TRDP_MD_CONFIG_T mdConfig = {
        md_cb,
        NULL,
        TRDP_MD_DEFAULT_SEND_PARAM,
        TRDP_FLAGS_NONE,
        5000000u,
        1000000u,
        60000000u,
        1000000u,
        0u,
        0u,
        32u};

    TRDP_APP_SESSION_T appHandle = NULL;
    TRDP_PUB_T pdPubHandle = NULL;
    TRDP_LIS_T mdListener = NULL;
    HMI_DOOR_CMD_PUBLISHER_T doorCmdPublishers[HMI_DOOR_COUNT];
    UINT8 doorAliveCounters[HMI_DOOR_COUNT];

    HMI_SUBSCRIPTION_T subscriptions[] = {
        {NULL, ownIp, "unicast"},
        {NULL, multicastA, "multicast-A"},
        {NULL, multicastB, "multicast-B"}};
    const UINT32 subCount = (UINT32)(sizeof(subscriptions) / sizeof(subscriptions[0]));

    UINT32 i;
    UINT8 pdTxPayload[HMI_PD_PAYLOAD_SIZE];
    UINT8 doorCmdTxPayload[HMI_DOOR_CMD_PAYLOAD_SIZE];
    UINT8 pdRxPayload[HMI_PD_PAYLOAD_SIZE];
    UINT32 tick = 0u;
    UINT32 maxTicks = 0u;
    int keepRunning = 1;

    if (runtimeSeconds > 0u)
    {
        maxTicks = (runtimeSeconds * 1000000u) / HMI_MAIN_LOOP_SLEEP_US;
    }

    memset(doorCmdPublishers, 0, sizeof(doorCmdPublishers));
    memset(doorAliveCounters, 0, sizeof(doorAliveCounters));

    if (tlc_init(log_cb, NULL, &memConfig) != TRDP_NO_ERR)
    {
        fprintf(stderr, "Failed to initialize TRDP stack\n");
        return 1;
    }

    if (tlc_openSession(&appHandle, ownIp, 0u, NULL, &pdConfig, &mdConfig, &processConfig) != TRDP_NO_ERR)
    {
        fprintf(stderr, "Failed to open TRDP session\n");
        tlc_terminate();
        return 1;
    }

    for (i = 0u; i < subCount; ++i)
    {
        TRDP_ERR_T err = add_pd_subscription(appHandle, &subscriptions[i], gatewayIp, HMI_PD_SUB_COMID);
        if (err != TRDP_NO_ERR)
        {
            fprintf(stderr, "Failed PD subscription (%s), err=%d\n", subscriptions[i].name, err);
            tlc_closeSession(appHandle);
            tlc_terminate();
            return 1;
        }
    }

    if (tlp_publish(appHandle,
                    &pdPubHandle,
                    NULL,
                    NULL,
                    0u,
                    HMI_PD_PUB_COMID,
                    0u,
                    0u,
                    ownIp,
                    gatewayIp,
                    HMI_PD_CYCLE_US,
                    0u,
                    TRDP_FLAGS_NONE,
                    NULL,
                    0u) != TRDP_NO_ERR)
    {
        fprintf(stderr, "Failed to create PD publisher\n");
        tlc_closeSession(appHandle);
        tlc_terminate();
        return 1;
    }

    for (i = 0u; i < HMI_DOOR_COUNT; ++i)
    {
        doorCmdPublishers[i].comId = HMI_DOOR_CMD_PD_COMID_BASE + i;
        if (tlp_publish(appHandle,
                        &doorCmdPublishers[i].handle,
                        NULL,
                        NULL,
                        0u,
                        doorCmdPublishers[i].comId,
                        0u,
                        0u,
                        ownIp,
                        gatewayIp,
                        HMI_PD_CYCLE_US,
                        0u,
                        TRDP_FLAGS_NONE,
                        NULL,
                        0u) != TRDP_NO_ERR)
        {
            fprintf(stderr, "Failed to create Door Command PD publisher for door %u\n", i);
            while (i > 0u)
            {
                --i;
                tlp_unpublish(appHandle, doorCmdPublishers[i].handle);
            }
            tlp_unpublish(appHandle, pdPubHandle);
            tlc_closeSession(appHandle);
            tlc_terminate();
            return 1;
        }
    }

    if (tlm_addListener(appHandle,
                        &mdListener,
                        NULL,
                        md_cb,
                        TRUE,
                        HMI_MD_RX_COMID,
                        0u,
                        0u,
                        gatewayIp,
                        gatewayIp,
                        VOS_INADDR_ANY,
                        TRDP_FLAGS_NONE,
                        NULL,
                        NULL) != TRDP_NO_ERR)
    {
        fprintf(stderr, "Failed to create MD listener\n");
        for (i = 0u; i < HMI_DOOR_COUNT; ++i)
        {
            if (doorCmdPublishers[i].handle != NULL)
            {
                tlp_unpublish(appHandle, doorCmdPublishers[i].handle);
            }
        }
        tlp_unpublish(appHandle, pdPubHandle);
        tlc_closeSession(appHandle);
        tlc_terminate();
        return 1;
    }

    printf("HMI started: own=%s gateway=%s\n", vos_ipDotted(ownIp), vos_ipDotted(gatewayIp));
    print_door_command_prompt();

    while (keepRunning)
    {
        TRDP_TIME_T tv = {0u, HMI_MAIN_LOOP_SLEEP_US};
        TRDP_FDS_T rfds;
        TRDP_SOCK_T noDesc = 0;
        INT32 count = 0;

        VOS_FD_ZERO(&rfds);
        (void)tlc_getInterval(appHandle, &tv, &rfds, &noDesc);
        if (noDesc > 0)
        {
            count = vos_select(noDesc, &rfds, NULL, NULL, &tv);
            if (count < 0)
            {
                count = 0;
            }
        }
        (void)tlc_process(appHandle, &rfds, &count);

        fill_demo_payload(pdTxPayload, sizeof(pdTxPayload), (UINT8)(tick & 0xFFu));
        if (tlp_put(appHandle, pdPubHandle, pdTxPayload, (UINT32)sizeof(pdTxPayload)) != TRDP_NO_ERR)
        {
            fprintf(stderr, "Warning: tlp_put failed\n");
        }

        {
            UINT32 selectedDoor = 0u;
            UINT8 selectedCmd = 0u;
            int commandResult = read_door_command(&selectedDoor, &selectedCmd);
            if (commandResult < 0)
            {
                keepRunning = 0;
            }
            else if (commandResult > 0)
            {
                TRDP_ERR_T doorPutResult;
                build_door_cmd_payload(doorCmdTxPayload, selectedCmd, doorAliveCounters[selectedDoor]);
                doorPutResult = tlp_put(appHandle,
                                        doorCmdPublishers[selectedDoor].handle,
                                        doorCmdTxPayload,
                                        (UINT32)sizeof(doorCmdTxPayload));
                if (doorPutResult != TRDP_NO_ERR)
                {
                    fprintf(stderr, "Warning: door command tlp_put failed for door %u, err=%d\n", selectedDoor, doorPutResult);
                }
                else
                {
                    printf("Door command sent: door=%u cmd=%u alive_counter=%u comId=%u\n",
                           selectedDoor,
                           selectedCmd,
                           doorAliveCounters[selectedDoor],
                           doorCmdPublishers[selectedDoor].comId);
                    ++doorAliveCounters[selectedDoor];
                }
                print_door_command_prompt();
            }
        }

        for (i = 0u; i < subCount; ++i)
        {
            TRDP_PD_INFO_T pdInfo;
            UINT32 dataSize = sizeof(pdRxPayload);
            if (tlp_get(appHandle, subscriptions[i].handle, &pdInfo, pdRxPayload, &dataSize) == TRDP_NO_ERR)
            {
                printf("PD rx (%s): comId=%u from=%s size=%u seq=%u\n",
                       subscriptions[i].name,
                       pdInfo.comId,
                       vos_ipDotted(pdInfo.srcIpAddr),
                       dataSize,
                       pdInfo.seqCount);
            }
        }

        ++tick;
        if (maxTicks > 0u && tick >= maxTicks)
        {
            break;
        }
    }

    tlm_delListener(appHandle, mdListener);
    for (i = 0u; i < HMI_DOOR_COUNT; ++i)
    {
        if (doorCmdPublishers[i].handle != NULL)
        {
            tlp_unpublish(appHandle, doorCmdPublishers[i].handle);
        }
    }
    tlp_unpublish(appHandle, pdPubHandle);
    for (i = 0u; i < subCount; ++i)
    {
        tlp_unsubscribe(appHandle, subscriptions[i].handle);
    }
    tlc_closeSession(appHandle);
    tlc_terminate();

    return 0;
}
