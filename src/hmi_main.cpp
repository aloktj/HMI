/*
 * HMI Web Application - Door Control Console
 *
 * Architecture:
 *   Thread 1: Crow web server (HTTP REST API + static page)
 *   Thread 2: TRDP communication loop (PD publish/subscribe + MD listener)
 *
 * Business Rules (derived from CAN ICD + requirements):
 *   - Speed == 0 km/h  -> doors may be commanded OPEN (cmd=1)
 *   - Speed  > 0 km/h  -> doors auto-commanded CLOSE (cmd=2), OPEN disabled
 *   - Emergency         -> all doors commanded OPEN regardless of speed
 *   - Obstruction       -> CLOSE button greyed out; door remains OPEN
 *   - alive_counter increments only when HMI command intent changes (per ICD §7a.iii)
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "crow_all.h"   /* Crow single-header — download from crowcpp.org */
#include "hmi_trdp.h"

/* ===================================================================
 * Shared application state (protected by g_mutex)
 * =================================================================== */
static std::mutex g_mutex;

/* Door status received from Gateway (read by web, written by TRDP thread) */
static AggregatedDoorStatus_T g_doorStatus;

/* Door commands to send to Gateway (written by web, read by TRDP thread) */
static AggregatedDoorCommand_T g_doorCmd;

/* Previous command snapshot for alive_counter change detection */
static uint8_t g_prevCmd[HMI_DOOR_COUNT];

/* Train speed and emergency state (written by web, read by TRDP thread) */
static uint32_t g_trainSpeed   = 0u;
static bool     g_emergency    = false;

/* Flag to stop TRDP thread */
static std::atomic<bool> g_running{true};

/* TRDP session handle (used only in TRDP thread, set once at init) */
static TRDP_APP_SESSION_T g_appHandle = nullptr;

/* ===================================================================
 * TRDP Callbacks
 * =================================================================== */
static void trdp_log_cb(void *pRefCon,
                         TRDP_LOG_T category,
                         const CHAR8 *pTime,
                         const CHAR8 *pFile,
                         UINT16 lineNumber,
                         const CHAR8 *pMsgStr)
{
    (void)pRefCon;
    static const char *cat[] = {"ERROR", "WARN", "INFO", "DEBUG", "USER"};
    const char *fn = strrchr(pFile, '/');
    printf("[TRDP-%s] %s %s:%u %s",
           cat[category], pTime, fn ? fn + 1 : pFile, lineNumber, pMsgStr);
}

static void trdp_md_cb(void *pRefCon,
                        TRDP_APP_SESSION_T appHandle,
                        const TRDP_MD_INFO_T *pMsg,
                        UINT8 *pData,
                        UINT32 dataSize)
{
    (void)pRefCon;
    (void)appHandle;
    printf("[MD] comId=%u msgType=0x%04x src=%s size=%u\n",
           pMsg->comId, pMsg->msgType,
           vos_ipDotted(pMsg->srcIpAddr), dataSize);
}

/* ===================================================================
 * Business Logic: apply speed / emergency rules to door commands
 * Must be called with g_mutex held.
 * =================================================================== */
static void apply_business_rules()
{
    for (uint32_t i = 0u; i < HMI_DOOR_COUNT; ++i)
    {
        uint8_t newCmd = g_doorCmd.doors[i].cmd;

        if (g_emergency)
        {
            /* Emergency: force all doors OPEN regardless of speed */
            newCmd = DOOR_CMD_OPEN;
        }
        else if (g_trainSpeed > 0u)
        {
            /* Train moving: force CLOSE on all doors */
            newCmd = DOOR_CMD_CLOSE;
        }
        /* else: speed == 0, no emergency -> keep user-selected command */

        /* Increment alive_counter only when command actually changes */
        if (newCmd != g_prevCmd[i])
        {
            g_doorCmd.doors[i].alive_counter++;
            g_prevCmd[i] = newCmd;
        }
        g_doorCmd.doors[i].cmd = newCmd;
    }
}

/* ===================================================================
 * TRDP Communication Thread
 * =================================================================== */
static void trdp_thread_func(UINT32 ownIp, UINT32 gatewayIp,
                              UINT32 multicastA, UINT32 multicastB)
{
    /* --- TRDP stack init --- */
    TRDP_MEM_CONFIG_T memConfig = {nullptr, 512000u, {0}};
    TRDP_PROCESS_CONFIG_T processConfig = {
        "HMI", "HMI TRDP WebApp", "",
        TRDP_PROCESS_DEFAULT_CYCLE_TIME, 0u, TRDP_OPTION_TRAFFIC_SHAPING, 0u
    };
    TRDP_PD_CONFIG_T pdConfig = {
        nullptr, nullptr, TRDP_PD_DEFAULT_SEND_PARAM,
        TRDP_FLAGS_NONE, HMI_PD_TIMEOUT_US, TRDP_TO_SET_TO_ZERO, 0u
    };
    TRDP_MD_CONFIG_T mdConfig = {
        trdp_md_cb, nullptr, TRDP_MD_DEFAULT_SEND_PARAM,
        TRDP_FLAGS_NONE, 5000000u, 1000000u, 60000000u, 1000000u, 0u, 0u, 32u
    };

    if (tlc_init(trdp_log_cb, nullptr, &memConfig) != TRDP_NO_ERR)
    {
        std::cerr << "TRDP init failed\n";
        g_running = false;
        return;
    }

    if (tlc_openSession(&g_appHandle, ownIp, 0u, nullptr,
                         &pdConfig, &mdConfig, &processConfig) != TRDP_NO_ERR)
    {
        std::cerr << "TRDP session open failed\n";
        tlc_terminate();
        g_running = false;
        return;
    }

    /* --- PD Subscriptions: aggregated door status --- */
    struct {
        TRDP_SUB_T handle;
        TRDP_IP_ADDR_T dest;
        const char *tag;
    } subs[] = {
        {nullptr, ownIp,      "unicast"},
        {nullptr, multicastA, "mcast-A"},
        {nullptr, multicastB, "mcast-B"},
    };
    const uint32_t subCount = sizeof(subs) / sizeof(subs[0]);

    for (uint32_t i = 0; i < subCount; ++i)
    {
        if (tlp_subscribe(g_appHandle, &subs[i].handle, nullptr, nullptr, 0u,
                          HMI_PD_DOOR_STATUS_COMID, 0u, 0u,
                          gatewayIp, gatewayIp, subs[i].dest,
                          TRDP_FLAGS_NONE, HMI_PD_TIMEOUT_US,
                          TRDP_TO_SET_TO_ZERO) != TRDP_NO_ERR)
        {
            std::cerr << "PD subscribe failed: " << subs[i].tag << "\n";
            tlc_closeSession(g_appHandle);
            tlc_terminate();
            g_running = false;
            return;
        }
    }

    /* --- PD Publisher: aggregated door command (HMI -> Gateway) --- */
    TRDP_PUB_T doorCmdPub = nullptr;
    if (tlp_publish(g_appHandle, &doorCmdPub, nullptr, nullptr, 0u,
                    HMI_PD_DOOR_CMD_COMID, 0u, 0u,
                    ownIp, gatewayIp, HMI_PD_CYCLE_US, 0u,
                    TRDP_FLAGS_NONE, nullptr, 0u) != TRDP_NO_ERR)
    {
        std::cerr << "PD publish (door cmd) failed\n";
        tlc_closeSession(g_appHandle);
        tlc_terminate();
        g_running = false;
        return;
    }

    /* --- PD Publisher: HMI heartbeat status --- */
    TRDP_PUB_T hmiStatusPub = nullptr;
    if (tlp_publish(g_appHandle, &hmiStatusPub, nullptr, nullptr, 0u,
                    HMI_PD_HMI_STATUS_COMID, 0u, 0u,
                    ownIp, gatewayIp, HMI_PD_CYCLE_US, 0u,
                    TRDP_FLAGS_NONE, nullptr, 0u) != TRDP_NO_ERR)
    {
        std::cerr << "PD publish (HMI status) failed\n";
        tlp_unpublish(g_appHandle, doorCmdPub);
        tlc_closeSession(g_appHandle);
        tlc_terminate();
        g_running = false;
        return;
    }

    /* --- MD Listener: optional gateway commands to HMI --- */
    TRDP_LIS_T mdListener = nullptr;
    tlm_addListener(g_appHandle, &mdListener, nullptr, trdp_md_cb, TRUE,
                    HMI_MD_RX_COMID, 0u, 0u, gatewayIp, gatewayIp,
                    VOS_INADDR_ANY, TRDP_FLAGS_NONE, nullptr, nullptr);

    printf("[TRDP] Running: own=%s gw=%s\n",
           vos_ipDotted(ownIp), vos_ipDotted(gatewayIp));

    /* --- Main TRDP loop --- */
    uint8_t rxBuf[HMI_AGGREGATED_PD_SIZE];
    uint8_t hmiStatusBuf[HMI_HMI_STATUS_PD_SIZE];
    static uint8_t hmiAlive = 0u;

    while (g_running)
    {
        TRDP_TIME_T tv = {0u, HMI_TRDP_LOOP_SLEEP_US};
        TRDP_FDS_T rfds;
        TRDP_SOCK_T noDesc = 0;
        INT32 count = 0;

        VOS_FD_ZERO(&rfds);
        tlc_getInterval(g_appHandle, &tv, &rfds, &noDesc);
        if (noDesc > 0)
        {
            count = vos_select(noDesc, &rfds, nullptr, nullptr, &tv);
            if (count < 0) count = 0;
        }
        tlc_process(g_appHandle, &rfds, &count);

        /* --- Receive aggregated door status --- */
        for (uint32_t i = 0; i < subCount; ++i)
        {
            TRDP_PD_INFO_T pdInfo;
            UINT32 dataSize = sizeof(rxBuf);
            if (tlp_get(g_appHandle, subs[i].handle, &pdInfo,
                        rxBuf, &dataSize) == TRDP_NO_ERR &&
                dataSize == HMI_AGGREGATED_PD_SIZE)
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                std::memcpy(&g_doorStatus, rxBuf, sizeof(g_doorStatus));
            }
        }

        /* --- Apply business rules and publish door commands --- */
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            apply_business_rules();
            tlp_put(g_appHandle, doorCmdPub,
                    reinterpret_cast<const UINT8 *>(&g_doorCmd),
                    static_cast<UINT32>(sizeof(g_doorCmd)));
        }

        /* --- Publish HMI heartbeat --- */
        std::memset(hmiStatusBuf, 0, sizeof(hmiStatusBuf));
        hmiStatusBuf[0] = ++hmiAlive;
        tlp_put(g_appHandle, hmiStatusPub,
                hmiStatusBuf, static_cast<UINT32>(sizeof(hmiStatusBuf)));
    }

    /* --- Cleanup --- */
    if (mdListener) tlm_delListener(g_appHandle, mdListener);
    tlp_unpublish(g_appHandle, doorCmdPub);
    tlp_unpublish(g_appHandle, hmiStatusPub);
    for (uint32_t i = 0; i < subCount; ++i)
        tlp_unsubscribe(g_appHandle, subs[i].handle);
    tlc_closeSession(g_appHandle);
    tlc_terminate();
    printf("[TRDP] Stopped\n");
}

/* ===================================================================
 * Helper: load HTML file from disk, with fallback
 * =================================================================== */
static std::string load_web_file(const std::string &path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        return "<html><body><h1>Error: " + path + " not found</h1></body></html>";
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

/* ===================================================================
 * JSON builders
 * =================================================================== */
static std::string build_status_json()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    std::ostringstream js;
    js << "{\"speed\":" << g_trainSpeed
       << ",\"emergency\":" << (g_emergency ? "true" : "false")
       << ",\"doors\":[";
    for (uint32_t i = 0; i < HMI_DOOR_COUNT; ++i)
    {
        const auto &d = g_doorStatus.doors[i];
        const auto &c = g_doorCmd.doors[i];
        if (i > 0) js << ",";
        js << "{\"id\":" << i
           << ",\"state\":" << (int)d.door_state
           << ",\"obstruction\":" << (int)d.obstruction
           << ",\"last_cmd\":" << (int)d.last_cmd
           << ",\"close_blocked\":" << (int)d.close_blocked
           << ",\"status_counter\":" << (int)d.status_counter
           << ",\"hmi_cmd\":" << (int)c.cmd
           << ",\"alive_counter\":" << (int)c.alive_counter
           << "}";
    }
    js << "]}";
    return js.str();
}

/* ===================================================================
 * Main
 * =================================================================== */
int main(int argc, char **argv)
{
    /* --- Parse arguments --- */
    UINT32 ownIp      = vos_dottedIP("192.168.56.2");
    UINT32 gatewayIp  = vos_dottedIP("192.168.56.1");
    UINT32 multicastA = vos_dottedIP("239.192.0.1");
    UINT32 multicastB = vos_dottedIP("239.192.0.2");
    uint16_t webPort  = HMI_WEB_PORT;
    std::string webDir = "web";

    if (argc > 1) ownIp      = vos_dottedIP(argv[1]);
    if (argc > 2) gatewayIp  = vos_dottedIP(argv[2]);
    if (argc > 3) multicastA = vos_dottedIP(argv[3]);
    if (argc > 4) multicastB = vos_dottedIP(argv[4]);
    if (argc > 5) webPort    = static_cast<uint16_t>(std::stoi(argv[5]));
    if (argc > 6) webDir     = argv[6];

    if (argc > 7)
    {
        printf("Usage: %s [own_ip] [gw_ip] [mc_a] [mc_b] [web_port] [web_dir]\n", argv[0]);
        return 1;
    }

    /* --- Initialize shared state --- */
    std::memset(&g_doorStatus, 0, sizeof(g_doorStatus));
    std::memset(&g_doorCmd, 0, sizeof(g_doorCmd));
    std::memset(g_prevCmd, 0, sizeof(g_prevCmd));
    /* All doors start with state CLOSED (1) assumed safe default */
    for (uint32_t i = 0; i < HMI_DOOR_COUNT; ++i)
        g_doorStatus.doors[i].door_state = DOOR_STATE_CLOSED;

    /* --- Start TRDP thread --- */
    std::thread trdpThread(trdp_thread_func, ownIp, gatewayIp, multicastA, multicastB);

    /* --- Crow web server setup --- */
    crow::SimpleApp app;

    /* Serve main page */
    std::string indexHtml = load_web_file(webDir + "/index.html");
    CROW_ROUTE(app, "/")
    ([&indexHtml]()
    {
        crow::response resp(indexHtml);
        resp.set_header("Content-Type", "text/html; charset=utf-8");
        return resp;
    });

    /* GET /api/status — polled by frontend every 500ms */
    CROW_ROUTE(app, "/api/status")
    ([]()
    {
        crow::response resp(build_status_json());
        resp.set_header("Content-Type", "application/json");
        return resp;
    });

    /* POST /api/speed  body: {"speed": <uint>} */
    CROW_ROUTE(app, "/api/speed").methods("POST"_method)
    ([](const crow::request &req)
    {
        auto body = crow::json::load(req.body);
        if (!body || !body.has("speed"))
            return crow::response(400, "Missing speed");

        uint32_t speed = static_cast<uint32_t>(body["speed"].i());
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_trainSpeed = speed;
        }
        printf("[WEB] Speed set to %u km/h\n", speed);
        return crow::response(200, "{\"ok\":true}");
    });

    /* POST /api/emergency  body: {"active": true/false} */
    CROW_ROUTE(app, "/api/emergency").methods("POST"_method)
    ([](const crow::request &req)
    {
        auto body = crow::json::load(req.body);
        if (!body || !body.has("active"))
            return crow::response(400, "Missing active");

        bool active = body["active"].b();
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_emergency = active;
            if (active)
            {
                /* Immediately command all doors OPEN */
                for (uint32_t i = 0; i < HMI_DOOR_COUNT; ++i)
                    g_doorCmd.doors[i].cmd = DOOR_CMD_OPEN;
            }
        }
        printf("[WEB] Emergency %s\n", active ? "ACTIVATED" : "DEACTIVATED");
        return crow::response(200, "{\"ok\":true}");
    });

    /* POST /api/door/<id>/open */
    CROW_ROUTE(app, "/api/door/<uint>/open").methods("POST"_method)
    ([](uint32_t doorId)
    {
        if (doorId >= HMI_DOOR_COUNT)
            return crow::response(400, "Invalid door ID");

        std::lock_guard<std::mutex> lk(g_mutex);

        /* Speed must be 0 to open (unless emergency — handled by business rules) */
        if (g_trainSpeed > 0u && !g_emergency)
            return crow::response(403, "{\"error\":\"Train is moving, cannot open\"}");

        g_doorCmd.doors[doorId].cmd = DOOR_CMD_OPEN;
        printf("[WEB] Door %u -> OPEN\n", doorId);
        return crow::response(200, "{\"ok\":true}");
    });

    /* POST /api/door/<id>/close */
    CROW_ROUTE(app, "/api/door/<uint>/close").methods("POST"_method)
    ([](uint32_t doorId)
    {
        if (doorId >= HMI_DOOR_COUNT)
            return crow::response(400, "Invalid door ID");

        std::lock_guard<std::mutex> lk(g_mutex);

        /* Cannot close if obstructed (per CAN ICD §6c) */
        if (g_doorStatus.doors[doorId].obstruction == 1u)
            return crow::response(403, "{\"error\":\"Door obstructed, cannot close\"}");

        g_doorCmd.doors[doorId].cmd = DOOR_CMD_CLOSE;
        printf("[WEB] Door %u -> CLOSE\n", doorId);
        return crow::response(200, "{\"ok\":true}");
    });

    printf("[WEB] Starting on port %u, serving from %s/\n", webPort, webDir.c_str());
    app.port(webPort).multithreaded().run();

    /* --- Shutdown --- */
    g_running = false;
    if (trdpThread.joinable())
        trdpThread.join();

    return 0;
}
