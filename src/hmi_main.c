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

    HMI_SUBSCRIPTION_T subscriptions[] = {
        {NULL, ownIp, "unicast"},
        {NULL, multicastA, "multicast-A"},
        {NULL, multicastB, "multicast-B"}};
    const UINT32 subCount = (UINT32)(sizeof(subscriptions) / sizeof(subscriptions[0]));

    UINT32 i;
    UINT8 pdTxPayload[HMI_PD_PAYLOAD_SIZE];
    UINT8 mdTxPayload[HMI_MD_PAYLOAD_SIZE];
    UINT8 pdRxPayload[HMI_PD_PAYLOAD_SIZE];
    UINT32 tick = 0u;
    UINT32 maxTicks = 0u;

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
        tlp_unpublish(appHandle, pdPubHandle);
        tlc_closeSession(appHandle);
        tlc_terminate();
        return 1;
    }

    printf("HMI started: own=%s gateway=%s\n", vos_ipDotted(ownIp), vos_ipDotted(gatewayIp));

    for (;;)
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

        if ((tick % 10u) == 0u)
        {
            fill_demo_payload(mdTxPayload, sizeof(mdTxPayload), (UINT8)(0xA0u + (tick & 0x0Fu)));
            (void)tlm_notify(appHandle,
                             NULL,
                             NULL,
                             HMI_MD_TX_COMID,
                             0u,
                             0u,
                             ownIp,
                             gatewayIp,
                             TRDP_FLAGS_NONE,
                             NULL,
                             mdTxPayload,
                             (UINT32)sizeof(mdTxPayload),
                             NULL,
                             NULL);
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
    tlp_unpublish(appHandle, pdPubHandle);
    for (i = 0u; i < subCount; ++i)
    {
        tlp_unsubscribe(appHandle, subscriptions[i].handle);
    }
    tlc_closeSession(appHandle);
    tlc_terminate();

    return 0;
}
