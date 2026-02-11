#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

typedef enum
{
    DOOR_UNKNOWN = 0,
    DOOR_CLOSED = 1,
    DOOR_OPEN = 2,
    DOOR_MOVING = 3,
    DOOR_FAULT = 4
} DOOR_STATUS_T;

typedef enum
{
    CMD_NONE = 0,
    CMD_OPEN = 1,
    CMD_CLOSE = 2,
    CMD_STOP = 3
} DOOR_COMMAND_T;

typedef struct
{
    UINT8 doorId;
    DOOR_STATUS_T status;
} DOOR_STATE_T;

static const char *door_status_to_string(DOOR_STATUS_T status)
{
    switch (status)
    {
    case DOOR_CLOSED:
        return "CLOSED";
    case DOOR_OPEN:
        return "OPEN";
    case DOOR_MOVING:
        return "MOVING";
    case DOOR_FAULT:
        return "FAULT";
    case DOOR_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static const char *door_command_to_string(DOOR_COMMAND_T command)
{
    switch (command)
    {
    case CMD_OPEN:
        return "OPEN";
    case CMD_CLOSE:
        return "CLOSE";
    case CMD_STOP:
        return "STOP";
    case CMD_NONE:
    default:
        return "NONE";
    }
}

static void usage(const char *app)
{
    printf("Usage: %s [runtime_s] [own_ip] [gateway_ip] [mc_a] [mc_b]\n", app);
    printf("Defaults: 0 192.168.56.2 192.168.56.1 239.192.0.1 239.192.0.2\n");
}

static void print_help(void)
{
    printf("\nHMI commands:\n");
    printf("  status                 Show all door statuses\n");
    printf("  open <id>              Open one door (id: 1..%u)\n", HMI_DOOR_COUNT);
    printf("  close <id>             Close one door\n");
    printf("  stop <id>              Stop one door\n");
    printf("  open-all               Open all doors\n");
    printf("  close-all              Close all doors\n");
    printf("  stop-all               Stop all doors\n");
    printf("  help                   Show this help\n");
    printf("  quit                   Exit the HMI\n\n");
}

static void print_status_table(const DOOR_STATE_T *doors, UINT32 count)
{
    UINT32 i;
    printf("\nDoor status table\n");
    printf("-----------------\n");
    for (i = 0u; i < count; ++i)
    {
        printf("Door %u : %s\n", doors[i].doorId, door_status_to_string(doors[i].status));
    }
    printf("\n");
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

static int find_door_index(UINT8 doorId)
{
    if (doorId < 1u || doorId > HMI_DOOR_COUNT)
    {
        return -1;
    }
    return (int)(doorId - 1u);
}

static void update_status_from_payload(DOOR_STATE_T *doors,
                                       UINT32 doorCount,
                                       const UINT8 *payload,
                                       UINT32 payloadSize)
{
    if (payload == NULL || payloadSize < 2u)
    {
        return;
    }

    if (payload[0] == 0u)
    {
        UINT32 i;
        UINT32 available = payloadSize - 1u;
        UINT32 limit = available < doorCount ? available : doorCount;
        for (i = 0u; i < limit; ++i)
        {
            doors[i].status = (DOOR_STATUS_T)payload[1u + i];
        }
        return;
    }

    {
        int idx = find_door_index(payload[0]);
        if (idx >= 0)
        {
            doors[idx].status = (DOOR_STATUS_T)payload[1];
        }
    }
}

static TRDP_ERR_T send_door_command(TRDP_APP_SESSION_T appHandle,
                                    UINT32 ownIp,
                                    UINT32 gatewayIp,
                                    UINT8 doorId,
                                    DOOR_COMMAND_T command)
{
    UINT8 payload[HMI_MD_PAYLOAD_SIZE] = {0u};
    payload[0] = doorId;
    payload[1] = (UINT8)command;

    return tlm_notify(appHandle,
                      NULL,
                      NULL,
                      HMI_MD_TX_COMID,
                      0u,
                      0u,
                      ownIp,
                      gatewayIp,
                      TRDP_FLAGS_NONE,
                      NULL,
                      payload,
                      2u,
                      NULL,
                      NULL);
}

static int parse_door_id(const char *token, UINT8 *doorId)
{
    char *endPtr = NULL;
    unsigned long val;

    if (token == NULL || doorId == NULL)
    {
        return 0;
    }

    val = strtoul(token, &endPtr, 10);
    if (token == endPtr || *endPtr != '\0' || val == 0ul || val > HMI_DOOR_COUNT)
    {
        return 0;
    }

    *doorId = (UINT8)val;
    return 1;
}

static void trim_line(char *line)
{
    size_t len;
    if (line == NULL)
    {
        return;
    }
    len = strlen(line);
    while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r' || isspace((unsigned char)line[len - 1u])))
    {
        line[len - 1u] = '\0';
        --len;
    }
}

static int process_console_command(TRDP_APP_SESSION_T appHandle,
                                   UINT32 ownIp,
                                   UINT32 gatewayIp,
                                   DOOR_STATE_T *doors,
                                   UINT32 doorCount,
                                   const char *line)
{
    char buffer[128];
    char *cmd;
    char *arg;

    strncpy(buffer, line, sizeof(buffer) - 1u);
    buffer[sizeof(buffer) - 1u] = '\0';

    cmd = strtok(buffer, " \t");
    if (cmd == NULL)
    {
        return 1;
    }

    arg = strtok(NULL, " \t");

    if (strcmp(cmd, "help") == 0)
    {
        print_help();
        return 1;
    }
    if (strcmp(cmd, "status") == 0)
    {
        print_status_table(doors, doorCount);
        return 1;
    }
    if (strcmp(cmd, "quit") == 0)
    {
        return 0;
    }

    if ((strcmp(cmd, "open") == 0) || (strcmp(cmd, "close") == 0) || (strcmp(cmd, "stop") == 0))
    {
        UINT8 doorId;
        DOOR_COMMAND_T doorCmd = CMD_NONE;
        int idx;

        if (!parse_door_id(arg, &doorId))
        {
            printf("Invalid door id. Use 1..%u\n", HMI_DOOR_COUNT);
            return 1;
        }

        if (strcmp(cmd, "open") == 0)
        {
            doorCmd = CMD_OPEN;
            idx = find_door_index(doorId);
            if (idx >= 0)
            {
                doors[idx].status = DOOR_OPEN;
            }
        }
        else if (strcmp(cmd, "close") == 0)
        {
            doorCmd = CMD_CLOSE;
            idx = find_door_index(doorId);
            if (idx >= 0)
            {
                doors[idx].status = DOOR_CLOSED;
            }
        }
        else
        {
            doorCmd = CMD_STOP;
        }

        if (send_door_command(appHandle, ownIp, gatewayIp, doorId, doorCmd) != TRDP_NO_ERR)
        {
            printf("Failed to send %s command to door %u\n", door_command_to_string(doorCmd), doorId);
            return 1;
        }

        printf("Command sent: %s door %u\n", door_command_to_string(doorCmd), doorId);
        return 1;
    }

    if ((strcmp(cmd, "open-all") == 0) || (strcmp(cmd, "close-all") == 0) || (strcmp(cmd, "stop-all") == 0))
    {
        UINT8 doorId;
        DOOR_COMMAND_T doorCmd = CMD_NONE;

        if (strcmp(cmd, "open-all") == 0)
        {
            doorCmd = CMD_OPEN;
        }
        else if (strcmp(cmd, "close-all") == 0)
        {
            doorCmd = CMD_CLOSE;
        }
        else
        {
            doorCmd = CMD_STOP;
        }

        for (doorId = 1u; doorId <= HMI_DOOR_COUNT; ++doorId)
        {
            if (send_door_command(appHandle, ownIp, gatewayIp, doorId, doorCmd) != TRDP_NO_ERR)
            {
                printf("Failed to send %s command to door %u\n", door_command_to_string(doorCmd), doorId);
                continue;
            }

            if (doorCmd == CMD_OPEN)
            {
                doors[doorId - 1u].status = DOOR_OPEN;
            }
            else if (doorCmd == CMD_CLOSE)
            {
                doors[doorId - 1u].status = DOOR_CLOSED;
            }
        }

        printf("Command sent: %s to all doors\n", door_command_to_string(doorCmd));
        return 1;
    }

    printf("Unknown command: %s\n", cmd);
    print_help();
    return 1;
}

int main(int argc, char **argv)
{
    UINT32 runtimeSeconds = 0u;
    UINT32 ownIp = vos_dottedIP("192.168.56.2");
    UINT32 gatewayIp = vos_dottedIP("192.168.56.1");
    UINT32 multicastA = vos_dottedIP("239.192.0.1");
    UINT32 multicastB = vos_dottedIP("239.192.0.2");

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
        NULL,
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

    HMI_SUBSCRIPTION_T subscriptions[] = {
        {NULL, ownIp, "unicast"},
        {NULL, multicastA, "multicast-A"},
        {NULL, multicastB, "multicast-B"}};
    const UINT32 subCount = (UINT32)(sizeof(subscriptions) / sizeof(subscriptions[0]));

    DOOR_STATE_T doorStates[HMI_DOOR_COUNT];

    UINT32 i;
    UINT8 pdTxPayload[HMI_PD_PAYLOAD_SIZE] = {0u};
    UINT8 pdRxPayload[HMI_PD_PAYLOAD_SIZE] = {0u};
    UINT32 tick = 0u;
    UINT32 maxTicks = 0u;
    int runLoop = 1;

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

    subscriptions[0].destination = ownIp;
    subscriptions[1].destination = multicastA;
    subscriptions[2].destination = multicastB;

    for (i = 0u; i < HMI_DOOR_COUNT; ++i)
    {
        doorStates[i].doorId = (UINT8)(i + 1u);
        doorStates[i].status = DOOR_UNKNOWN;
    }

    if (runtimeSeconds > 0u)
    {
        maxTicks = (runtimeSeconds * 1000000u) / HMI_MAIN_LOOP_SLEEP_US;
    }

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

    if (tlm_addListener(appHandle,
                        &mdListener,
                        NULL,
                        NULL,
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
        tlp_unpublish(appHandle, pdPubHandle);
        tlc_closeSession(appHandle);
        tlc_terminate();
        return 1;
    }

    printf("HMI started: own=%s gateway=%s\n", vos_ipDotted(ownIp), vos_ipDotted(gatewayIp));
    print_help();
    print_status_table(doorStates, HMI_DOOR_COUNT);

    while (runLoop)
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

        pdTxPayload[0] = 0u;
        for (i = 0u; i < HMI_DOOR_COUNT && (1u + i) < sizeof(pdTxPayload); ++i)
        {
            pdTxPayload[1u + i] = (UINT8)doorStates[i].status;
        }

        if (tlp_put(appHandle, pdPubHandle, pdTxPayload, (UINT32)sizeof(pdTxPayload)) != TRDP_NO_ERR)
        {
            fprintf(stderr, "Warning: tlp_put failed\n");
        }

        for (i = 0u; i < subCount; ++i)
        {
            TRDP_PD_INFO_T pdInfo;
            UINT32 dataSize = sizeof(pdRxPayload);
            if (tlp_get(appHandle, subscriptions[i].handle, &pdInfo, pdRxPayload, &dataSize) == TRDP_NO_ERR)
            {
                update_status_from_payload(doorStates, HMI_DOOR_COUNT, pdRxPayload, dataSize);
                printf("PD update (%s): comId=%u from=%s size=%u\n",
                       subscriptions[i].name,
                       pdInfo.comId,
                       vos_ipDotted(pdInfo.srcIpAddr),
                       dataSize);
                print_status_table(doorStates, HMI_DOOR_COUNT);
            }
        }

        {
            fd_set stdinFds;
            struct timeval stdinTv = {0, 0};
            char line[128];

            FD_ZERO(&stdinFds);
            FD_SET(STDIN_FILENO, &stdinFds);

            if (select(STDIN_FILENO + 1, &stdinFds, NULL, NULL, &stdinTv) > 0 && FD_ISSET(STDIN_FILENO, &stdinFds))
            {
                if (fgets(line, sizeof(line), stdin) == NULL)
                {
                    runLoop = 0;
                }
                else
                {
                    trim_line(line);
                    runLoop = process_console_command(appHandle, ownIp, gatewayIp, doorStates, HMI_DOOR_COUNT, line);
                }
            }
        }

        ++tick;
        if (maxTicks > 0u && tick >= maxTicks)
        {
            break;
        }
    }

    tlm_delListener(appHandle, mdListener);
    tlp_unpublish(appHandle, pdPubHandle);
    for (i = 0u; i < subCount; ++i)
    {
        tlp_unsubscribe(appHandle, subscriptions[i].handle);
    }
    tlc_closeSession(appHandle);
    tlc_terminate();

    return 0;
}
