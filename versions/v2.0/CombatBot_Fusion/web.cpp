/**
 ******************************************************************************
 * @file    web.cpp
 * @brief   HTTP / WebSocket / REST 三通道服务实现
 *
 * @details 模块内部由四部分组成：
 *            1. 遥测编码器  @ref buildTelemetry —— 把 @ref Telemetry 压成精简 JSON；
 *            2. 回传泵      @ref pumpDone       —— 把控制核的完成/标定/告警事件广播出去；
 *            3. 指令分发    @ref dispatchAction / @ref handleWsCmd —— JSON → @ref Cmd；
 *            4. 路由挂载    @ref webInit        —— HTTP + WS + REST 全部注册点。
 *
 *          为什么遥测要「精简」：
 *            WebSocket 帧在 ESP32 上每帧都要走一次 LwIP 拷贝，字段过多会明显抬高
 *            网络核占用，进而挤压 WiFi 协议栈的时间片。因此这里统一做了定点数截断
 *            （转速/速度 1 位小数、电压 2 位小数、角度 1 位小数），既够用又省带宽。
 *
 * @author  HJZ
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>源文件由 src/ 平铺至根目录；
 *                                                    统一企业级注释规范
 *          </table>
 *
 * @warning 本文件全部代码运行于网络核，**禁止**在此直接访问电机 / 传感器 / IMU。
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

#include "cfg.h"
#include "robot.h"
#include "wifi_mgr.h"
#include "web.h"
#include "web_ui.h"

/* ==========================================================================
 *                          私有宏常量定义
 * ========================================================================== */

/** @brief HTTP 服务端口（固定 80，便于手机直接输入 IP 访问）            */
#define WEB_HTTP_PORT           80

/** @brief WebSocket 客户端清理周期 ms（防止断开的句柄长期占用内存）     */
#define WEB_CLEANUP_MS          5000

/** @brief 遥测频率配置的刷新周期 ms（避免每次主循环都去抢配置互斥量）   */
#define WEB_CFG_POLL_MS         1000

/** @brief REST 请求体上限，超出直接丢弃，防止恶意大包打爆堆内存         */
#define WEB_BODY_MAX            4096

/**
 * @brief REST 通道的「虚拟连接编号」
 * @details AsyncWebSocket 的真实客户端编号从 1 开始递增，因此 0 天然表示
 *          "无连接"。REST/HTTP 是无状态通道，拿不到连接号，这里给它分配一个
 *          固定的合成编号，使其同样能参与控制权仲裁（而不是被判为无主指令）。
 *          取 0xFFFFFFFF 是刻意避开真实编号区间，便于网页侧区分来源。
 */
#define WEB_CLIENT_REST         0xFFFFFFFFu

/* ==========================================================================
 *                          私有变量（文件内静态）
 * ========================================================================== */

static AsyncWebServer s_server(WEB_HTTP_PORT);  /**< HTTP 服务器实例      */
static AsyncWebSocket s_ws("/ws");              /**< WebSocket 实例       */

static String    s_body;         /**< POST 请求体累积缓冲（分片接收）     */
static uint32_t  s_lastTm = 0;   /**< 上次遥测推送时间戳                  */
static uint32_t  s_lastClean = 0;/**< 上次客户端清理时间戳                */
static uint32_t  s_lastCfg = 0;  /**< 上次刷新遥测频率的时间戳            */
static uint16_t  s_tmPeriod = 66;/**< 遥测推送周期 ms（默认 15Hz）        */

/* ==========================================================================
 *                          私有函数（文件内静态）
 * ========================================================================== */

/**
 * @brief   截断浮点到指定小数位（用于压缩遥测报文）
 * @param[in] v     原始值
 * @param[in] mult  10^小数位（10=1位，100=2位）
 * @return  截断后的值
 */
static float roundTo(float v, float mult) {
    return roundf(v * mult) / mult;
}

/**
 * @brief   向全部在线 WS 客户端广播一条文本帧
 * @param[in] msg  已序列化的 JSON 字符串
 * @return   无
 * @note     客户端数为 0 时底层会直接返回，不会有任何内存分配。
 */
static void wsBroadcast(const String& msg) {
    s_ws.textAll(msg);
}

/**
 * @brief   把告警码翻译成人话（显示在网页的 toast 上）
 * @param[in] sub  告警子码，取值见 @ref DoneType 的 D_ALERT 分支
 * @return  中文提示字符串（静态存储期，无需释放）
 */
static const char* alertText(uint8_t sub) {
    switch (sub) {
        case 1:  return "已触发急停，请解锁后继续";
        case 2:  return "检测到电机堵转，已停机";
        case 3:  return "电池严重低压，已停机";
        case 4:  return "检测到台面边缘，已刹车";
        case 5:  return "心跳超时（控制端失联），已停车";
        case 6:  return "仍处于危险状态（边缘/严重低压），拒绝解锁";
        case 7:  return "零偏标定失败：采样期间车辆未静止，请重试";
        default: return "安全告警";
    }
}

/**
 * @brief   把当前遥测快照编码为一帧 JSON
 * @param   无
 * @return  JSON 字符串，形如 {"t":"tm","rpm":[..],"spd":..,"yaw":..}
 * @note    字段命名与 web_ui.h 中的 onMsg() 严格对应，改字段名必须同步改网页。
 */
static String buildTelemetry() {
    Telemetry t;
    robotCopyTelemetry(t);

    JsonDocument doc;

    doc["t"] = "tm";

    /* ---- 四轮转速（1 位小数，rpm，带符号） ---- */
    JsonArray rpm = doc["rpm"].to<JsonArray>();
    for (int i = 0; i < 4; i++) rpm.add(roundTo(t.rpm[i], 10.0f));

    /* ---- 车体运动状态 ---- */
    doc["spd"] = roundTo(t.spd, 10.0f);       /* 线速度 cm/s              */
    doc["yaw"] = roundTo(t.yaw, 10.0f);       /* 融合偏航角 °             */
    doc["yr"]  = roundTo(t.yawRate, 10.0f);   /* 偏航角速度 °/s           */
    doc["odo"] = roundTo(t.odoCm, 10.0f);     /* 累计里程 cm              */

    /* ---- 六路红外测距（cm，超出量程会返回上限值） ----
       ★ 融合版：无效通道下发 JSON null，网页显示 "--"。
       宁可显示"无数据"，也不要把一个坏掉的传感器伪装成 18cm。 ---- */
    JsonArray ir = doc["ir"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        if (t.irValid[i]) ir.add(roundTo(t.ir[i], 10.0f));
        else              ir.add(nullptr);
    }

    /* ---- 开关量：灰度 4 路 + 光电 3 路（统一 0/1，网页直接当布尔用） ---- */
    JsonArray gr = doc["gr"].to<JsonArray>();
    for (int i = 0; i < 4; i++) gr.add(t.gray[i] ? 1 : 0);
    JsonArray e18 = doc["e18"].to<JsonArray>();
    for (int i = 0; i < 3; i++) e18.add(t.e18[i] ? 1 : 0);

    /* ---- 姿态：三轴加速度（g）+ 俯仰/横滚（°） ---- */
    JsonArray acc = doc["acc"].to<JsonArray>();
    for (int i = 0; i < 3; i++) acc.add(roundTo(t.acc[i], 100.0f));
    doc["pit"] = roundTo(t.pitch, 10.0f);
    doc["rol"] = roundTo(t.roll, 10.0f);
    doc["imu"] = (t.mpuOK || t.adxlOK) ? 1 : 0;   /* 顶栏 IMU 在线指示灯 */

    /* ---- 电源与安全标志 ---- */
    if (t.batValid) doc["bat"] = roundTo(t.bat, 100.0f);   /* 电池电压 V */
    else            doc["bat"] = nullptr;                  /* 分压未接   */
    doc["es"]  = t.estop  ? 1 : 0;                 /* 急停锁定             */
    doc["lb"]  = t.lowBat ? 1 : 0;                 /* 低压告警             */
    doc["ed"]  = t.edge   ? 1 : 0;                 /* 边缘触发             */
    doc["sl"]  = (t.senStale || !t.ioOk) ? 1 : 0;  /* 传感器陈旧 / IO 离线 */
    /* 控制权归属：0=空闲；0xFFFFFFFF=REST 通道占用；其余=WS 连接编号 */
    doc["own"] = t.owner;

    /* ---- 动作状态：模式 / 状态 / 进度 ---- */
    doc["md"] = t.mode;
    doc["st"] = t.state;
    doc["pg"] = roundTo(t.progress, 100.0f);

    /* ---- 舵机与标定 ---- */
    doc["sv"]  = t.servoUS;                        /* 当前脉宽 µs          */
    doc["cal"] = t.calPending;                     /* 等待回填的标定项     */

    /* ---- 网络信息：让顶栏能显示 AP / AP+STA 与信号强度 ---- */
    doc["net"]  = wifiModeStr();
    doc["rssi"] = wifiRSSI();

    /* ---- 运行时间（秒），用于网页显示 Uptime ---- */
    doc["up"] = (uint32_t)(t.uptime / 1000);

    String out;
    serializeJson(doc, out);
    return out;
}

/**
 * @brief   取空回传队列并把每条事件广播给所有客户端
 * @param   无
 * @return  无
 * @note    事件分三类，网页 onMsg() 按 t 字段分流：
 *            - "done"  精准动作完成（type = move / turn）
 *            - "cal"   标定流程（wait 等待回填 / ok 系数解算完成 / imu 零偏写入）
 *            - "alert" 安全告警（msg 为中文提示）
 */
static void pumpDone() {
    DoneMsg d;

    while (robotPopDone(d)) {
        JsonDocument doc;

        switch (d.type) {
            case D_MOVE:                                  /* 精准直线完成 */
            case D_TURN:                                  /* 精准转向完成 */
                doc["t"]      = "done";
                doc["type"]   = (d.type == D_MOVE) ? "move" : "turn";
                doc["target"] = roundTo(d.target, 100.0f);
                doc["actual"] = roundTo(d.actual, 100.0f);
                doc["err"]    = roundTo(d.err, 100.0f);
                break;

            case D_CAL_IMU:                               /* 零偏采样完成 */
                doc["t"]    = "cal";
                doc["mode"] = "imu";
                doc["bias"] = d.actual;
                break;

            case D_CAL_ODO_WAIT:                          /* 等待实测距离 */
            case D_CAL_TURN_WAIT:                         /* 等待实测角度 */
                doc["t"]    = "cal";
                doc["mode"] = (d.type == D_CAL_ODO_WAIT) ? "odo" : "turn";
                doc["wait"] = 1;
                doc["cmd"]  = roundTo(d.target, 100.0f);
                break;

            case D_CAL_RESULT:                            /* 系数解算完成 */
                doc["t"]     = "cal";
                doc["mode"]  = (d.sub == CAL_TURN) ? "turn" : "odo";
                doc["ok"]    = 1;
                doc["value"] = d.actual;
                break;

            case D_ALERT:                                 /* 安全告警     */
                doc["t"]   = "alert";
                doc["msg"] = alertText(d.sub);
                break;

            default:
                break;
        }

        String out;
        serializeJson(doc, out);
        if (out.length() > 2) wsBroadcast(out);
    }
}

/**
 * @brief   把「动作名 + 参数」翻译成控制核指令并投递
 * @param[in] act  动作名：stop / move / turn / servo / estop / unlock / cal
 * @param[in] a    参数 A（距离 cm 或角度 °）
 * @param[in] u    参数 U（速度百分比 / 舵机位置 / 标定模式）
 * @param[in] client 发起方连接编号（WS 为真实编号，REST 为 WEB_CLIENT_REST）
 * @return  true 指令合法且已投递；false 动作名无法识别
 * @note    本函数是 REST /api/action 与 /api/calibrate 的公共出口。
 */
static bool dispatchAction(const String& act, float a, uint8_t u, uint32_t client) {
    Cmd c;
    c.t = C_NOP;
    c.a = 0;
    c.b = 0;
    c.u = 0;
    c.client = client;

    if      (act == "stop")   { c.t = C_STOP;                    }
    else if (act == "move")   { c.t = C_MOVE;  c.a = a; c.u = u; }
    else if (act == "turn")   { c.t = C_TURN;  c.a = a;          }
    else if (act == "servo")  { c.t = C_SERVO; c.u = u;          }
    else if (act == "estop")  { c.t = C_ESTOP;                   }
    else if (act == "unlock") { c.t = C_UNLOCK;                  }
    else if (act == "cal")    { c.t = C_CAL;   c.u = u;          }
    else                      { return false;                    }

    return robotPushCmd(c);
}

/**
 * @brief   解析一条上行指令 JSON 并投递到控制核
 * @param[in] doc     已反序列化的指令文档
 * @param[in] client  发起方连接编号（用于控制权仲裁）
 * @return  true 识别成功；false 未知指令（调用方应回复 400）
 * @note    支持的报文（与 web_ui.h 的 send() 一一对应）：
 *          @verbatim
 *              {"t":"drv","x":-1.0~1,"y":-1.0~1,"spd":0~100}
 *              {"t":"hb"}                      仅刷新心跳，不改变当前输入
 *              {"t":"stop"}                    立即停车并退出连续模式
 *              {"t":"move","dist":cm,"spd":%}  精准直线（负距离=后退）
 *              {"t":"turn","deg":°}            精准转向（正=逆时针）
 *              {"t":"servo","pos":0|1}         0=放铲 1=举铲
 *              {"t":"estop"} / {"t":"unlock"}  急停锁定 / 解锁
 *              {"t":"cal","mode":"imu|odo|turn"[,"actual":实测值]}
 *              {"t":"ping"}
 *          @endverbatim
 */
static bool handleCmdJson(JsonDocument& doc, uint32_t client) {
    String t = doc["t"] | "";
    if (t.length() == 0) t = doc["act"] | "";   /* REST 通道用 act 字段 */

    /* ---- 连续驱动：单独处理，因为它带 x/y 两个参数 ---- */
    if (t == "drv") {
        Cmd c;
        c.t = C_DRV;
        c.a = doc["x"]   | 0.0f;
        c.b = doc["y"]   | 0.0f;
        c.u = (uint8_t)(doc["spd"] | 60);
        c.client = client;
        return robotPushCmd(c);
    }

    /* ---- 心跳：只刷新看门狗，不覆盖当前摇杆输入 ---- */
    if (t == "hb") {
        Cmd c;
        c.t = C_HB;
        c.a = 0; c.b = 0; c.u = 0;
        c.client = client;
        return robotPushCmd(c);
    }

    /* ---- 标定：带 actual 表示「回填实测值」，否则表示「开始标定」 ---- */
    if (t == "cal") {
        String m = doc["mode"] | "";
        uint8_t mode = (m == "imu") ? CAL_IMU : ((m == "turn") ? CAL_TURN : CAL_ODO);

        Cmd c;
        c.b = 0;
        c.client = client;

        /* ArduinoJson 7 已废弃 containsKey()，改用 isNull() 判存在性：
           键不存在时 operator[] 返回的是 null 值，isNull() 为 true。 */
        if (!doc["actual"].isNull()) {            /* 第二步：解算系数     */
            c.t = C_CAL_FIN;
            c.a = doc["actual"] | 0.0f;
            c.u = mode;
        } else {                                   /* 第一步：跑标定动作   */
            c.t = C_CAL;
            c.a = 0;
            c.u = mode;
        }
        return robotPushCmd(c);
    }

    /* ---- 单参数动作：交给公共分发器 ---- */
    float   a = 0;
    uint8_t u = 0;
    if (t == "move")  { a = doc["dist"] | 0.0f; u = (uint8_t)(doc["spd"] | 0); }
    else if (t == "turn")  { a = doc["deg"]  | 0.0f; }
    else if (t == "servo") { u = (uint8_t)(doc["pos"] | 0); }

    if (t == "ping") return true;                 /* ping 不产生任何副作用 */

    return dispatchAction(t, a, u, client);
}

/**
 * @brief   WebSocket 事件回调（网络核上下文）
 * @param[in] server  WebSocket 服务器实例
 * @param[in] client  触发事件的客户端（CONNECT 时为新客户端）
 * @param[in] type    事件类型
 * @param[in] arg     帧信息（仅 DATA 事件有效）
 * @param[in] data    数据负载
 * @param[in] len     数据长度
 * @return   无
 * @warning  DISCONNECT 里必须立即停车 —— 这是「失联即停」安全链的第一道闸门。
 */
static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
    (void)server;

    switch (type) {
        case WS_EVT_CONNECT: {
            /* 新连接立刻补一帧遥测：否则页面要等到下一个推送周期才有数据，
               用户会误以为设备离线（15Hz 下最多 66ms，观感仍然明显）。
               ★ 融合版：握手帧带上本连接的编号，网页据此判断
                 「控制权是不是在自己手上」（见 web_ui.h 的 #owner 徽章）。 */
            JsonDocument doc;
            doc["t"]  = "hello";
            doc["id"] = client->id();
            String hello;
            serializeJson(doc, hello);
            client->text(hello);
            client->text(buildTelemetry());
            break;
        }

        case WS_EVT_DISCONNECT: {
            /* ★ 融合版：只有"当前控制者"断开才需要停车。
               旁观者的页面关掉不应打断正在进行的控制，否则多开一个页面
               就会把车停死——这是早期版本最容易踩的坑。 */
            robotDisconnect(client->id());
            break;
        }

        case WS_EVT_DATA: {
            AwsFrameInfo* info = static_cast<AwsFrameInfo*>(arg);

            /* 本项目所有报文都在 1KB 以内，不处理分片：
               final=false 或 index!=0 的分片直接丢弃，避免半包解析。 */
            if (info == nullptr || !info->final || info->index != 0 || info->len != len) return;
            if (info->opcode != WS_TEXT) return;                 /* 只接受文本帧 */
            if (len == 0 || len > WEB_BODY_MAX) return;          /* 长度哨兵     */

            /* data 不带结尾 '\0'，必须先落地成 String 再交给 ArduinoJson */
            String msg;
            msg.reserve(len + 1);
            for (size_t i = 0; i < len; i++) msg += static_cast<char>(data[i]);

            JsonDocument doc;
            if (deserializeJson(doc, msg)) return;               /* 非法 JSON 静默丢弃 */

            if (!handleCmdJson(doc, client->id())) {
                String err = "{\"t\":\"alert\",\"msg\":\"未知指令\"}";
                client->text(err);
            }
            break;
        }

        case WS_EVT_PONG:
        case WS_EVT_ERROR:
        default:
            break;
    }
}

/**
 * @brief   POST 请求体分片接收回调（REST 通道共用）
 * @param[in] request  当前请求
 * @param[in] data     本次分片数据
 * @param[in] len      本次分片长度
 * @param[in] index    本分片在整个报文中的起始偏移
 * @param[in] total    报文总长度
 * @param[in] handler  收齐后调用的处理函数（参数为完整报文）
 * @return   无
 */
typedef void (*BodyHandler)(AsyncWebServerRequest* request, const String& body);

static void onBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                   size_t index, size_t total, BodyHandler handler) {
    /* 超大报文直接掐断：防止恶意请求把堆内存吃光导致控制核复位 */
    if (total > WEB_BODY_MAX) {
        s_body = "";
        request->send(413, "application/json", "{\"ok\":false,\"err\":\"body too large\"}");
        return;
    }

    if (index == 0) s_body = "";                  /* 第一片：清空缓冲     */

    for (size_t i = 0; i < len; i++) s_body += static_cast<char>(data[i]);

    if (index + len == total) {                   /* 最后一片：交付处理   */
        String body = s_body;
        s_body = "";
        handler(request, body);
    }
}

/**
 * @brief   处理 /api/config 的 POST 报文
 * @param[in] request  当前请求
 * @param[in] body     完整 JSON 报文
 * @return   无
 * @note     支持两种特殊报文：
 *            - {"__reset":1} —— 恢复出厂默认值；
 *            - 普通配置对象  —— 部分更新（只覆盖出现的键）。
 */
static void handleCfgPost(AsyncWebServerRequest* request, const String& body) {
    /* ---- 恢复出厂：优先级最高，命中后立即返回 ---- */
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
        int reset = doc["__reset"] | 0;
        if (reset) {
            cfgFactoryReset();
            request->send(200, "application/json", "{\"ok\":true,\"reset\":true}");
            return;
        }
    }

    /* ---- 普通配置更新 ----
       ★ 融合版：车辆正在动作时拒绝改参数。理由很实际——
         改 PID / 最大速度会让正在执行的动作突然换一套闭环参数，
         轻则超调，重则直接冲出去。 */
    if (robotBusy()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"err\":\"busy: 动作执行中，请先停车\"}");
        return;
    }

    /* 写 NVS 期间进入「占用保持」：这期间新动作抢不到控制权，
       保证保存过程不会被并发指令打断。 */
    robotHold(true);
    const bool ok = cfgFromJson(body);
    if (ok) {
        cfgSave();                                /* 落盘 NVS，掉电不丢   */
        wifiApplySta();                           /* STA 凭据可能已变更   */
    }
    robotHold(false);

    if (ok) {
        request->send(200, "application/json", "{\"ok\":true}");
    } else {
        request->send(400, "application/json", "{\"ok\":false,\"err\":\"bad json\"}");
    }
}

/**
 * @brief   处理 /api/action 的 POST 报文（REST 动作通道，便于脚本调试）
 * @param[in] request  当前请求
 * @param[in] body     完整 JSON 报文，如 {"act":"move","dist":50,"spd":60}
 * @return   无
 */
static void handleActionPost(AsyncWebServerRequest* request, const String& body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        request->send(400, "application/json", "{\"ok\":false,\"err\":\"bad json\"}");
        return;
    }

    if (handleCmdJson(doc, WEB_CLIENT_REST)) {
        request->send(200, "application/json", "{\"ok\":true}");
    } else {
        request->send(400, "application/json", "{\"ok\":false,\"err\":\"unknown action\"}");
    }
}

/**
 * @brief   处理 /api/calibrate 的 POST 报文（标定专用通道）
 * @param[in] request  当前请求
 * @param[in] body     完整 JSON 报文，如 {"mode":"odo","actual":97.5}
 * @return   无
 * @note     与 WebSocket 的 cal 报文共用同一套 @ref handleCmdJson 逻辑。
 */
static void handleCalPost(AsyncWebServerRequest* request, const String& body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        request->send(400, "application/json", "{\"ok\":false,\"err\":\"bad json\"}");
        return;
    }

    doc["t"] = "cal";                             /* 统一成内部指令格式   */

    if (handleCmdJson(doc, WEB_CLIENT_REST)) {
        request->send(200, "application/json", "{\"ok\":true}");
    } else {
        request->send(400, "application/json", "{\"ok\":false,\"err\":\"bad mode\"}");
    }
}

/* ==========================================================================
 *                              公开函数实现
 * ========================================================================== */

/**
 * @brief   初始化 Web 服务（HTTP 服务器 + WebSocket + 全部 REST 路由）
 * @param   无
 * @return  无
 * @note    路由一览：
 *          @verbatim
 *              GET  /              内嵌网页（PROGMEM）
 *              WS   /ws            实时指令 / 遥测
 *              GET  /api/config    读取配置 JSON
 *              POST /api/config    写入配置（支持 {"__reset":1}）
 *              POST /api/action    触发动作
 *              POST /api/calibrate 触发 / 回填标定
 *              GET  /api/status    设备状态摘要
 *              POST /api/reboot    重启设备
 *          @endverbatim
 */
void webInit() {
    /* ---- WebSocket：先挂 handler 再注册到服务器，顺序不可颠倒 ---- */
    s_ws.onEvent(onWsEvent);
    s_server.addHandler(&s_ws);

    /* ---- 页面通道：整页由 PROGMEM 直出，不占用堆内存 ---- */
    s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        /* 两代异步 Web 库的整页下发方式不同，这里按编译期宏自动切换：
             - ESP32Async v3.x（core 3.x 必须用这一版）：send_P 已被标记 deprecated，
               改用 send() 的「指针 + 显式长度」重载；
             - me-no-dev v1.2.x（core 2.x）：只有 send_P 能零拷贝直发 Flash。
           两条分支都不经过 String，避免 47KB 网页在堆上再复制一份。 */
#if defined(ASYNCWEBSERVER_FORK_ESP32Async)
        request->send(200, "text/html",
                      reinterpret_cast<const uint8_t*>(WEB_UI_HTML),
                      sizeof(WEB_UI_HTML) - 1);
#else
        request->send_P(200, "text/html",
                        reinterpret_cast<const uint8_t*>(WEB_UI_HTML),
                        sizeof(WEB_UI_HTML) - 1);
#endif
    });

    /* ---- 配置通道：读 ---- */
    s_server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", cfgToJson());
    });

    /* ---- 配置通道：写（分片接收） ---- */
    s_server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* request) { (void)request; },   /* 首部回调：占位 */
        nullptr,                                                 /* 上传回调：无   */
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len,
           size_t index, size_t total) {
            onBody(request, data, len, index, total, handleCfgPost);
        });

    /* ---- 动作通道 ---- */
    s_server.on("/api/action", HTTP_POST,
        [](AsyncWebServerRequest* request) { (void)request; },
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len,
           size_t index, size_t total) {
            onBody(request, data, len, index, total, handleActionPost);
        });

    /* ---- 标定通道 ---- */
    s_server.on("/api/calibrate", HTTP_POST,
        [](AsyncWebServerRequest* request) { (void)request; },
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len,
           size_t index, size_t total) {
            onBody(request, data, len, index, total, handleCalPost);
        });

    /* ---- 状态摘要：供网页顶栏显示 mDNS / IP / 运行时间 ---- */
    s_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        CfgSnap cs;
        JsonDocument doc;
        doc["mdns"]   = cs.c.mdns;
        doc["ap"]     = wifiAPIP();
        doc["sta"]    = wifiSTAIP();
        doc["active"] = wifiActiveIP();
        doc["net"]    = wifiModeStr();
        doc["rssi"]   = wifiRSSI();
        doc["up"]     = (uint32_t)(millis() / 1000);
        doc["heap"]   = (uint32_t)ESP.getFreeHeap();

        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    /* ---- 重启：先回响应再重启，否则客户端会收到连接重置 ---- */
    s_server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", "{\"ok\":true}");
        delay(100);                               /* 让响应帧先发出去     */
        ESP.restart();
    });

    /* ---- 404：强制门户开启时统一重定向到首页（手机连 AP 自动弹页面） ---- */
    s_server.onNotFound([](AsyncWebServerRequest* request) {
        if (wifiCaptive()) {
            /* 用 c_str() 而不是直接传 String：两代库的 redirect 重载不一致，
               传 const char* 是唯一在 v1.2.x 与 v3.x 都成立的形式。 */
            String url = "http://" + wifiActiveIP() + "/";
            request->redirect(url.c_str());
        } else {
            request->send(404, "text/plain", "Not Found");
        }
    });

    s_server.begin();
}

/**
 * @brief   Web 服务周期任务（主循环调用，非阻塞）
 * @param[in] now  当前毫秒时间戳（millis()）
 * @return   无
 * @note     三段逻辑各自独立计时，互不影响：
 *             ① 1s 刷新一次遥测周期（配置可能被网页改过）；
 *             ② 到点推送遥测（无客户端时跳过序列化，省 CPU）；
 *             ③ 每帧都泵一次回传队列（事件必须尽快送达，不能等推送周期）；
 *             ④ 5s 清理断开客户端（AsyncWebSocket 不会自动回收句柄）。
 */
void webLoop(uint32_t now) {
    /* ① 遥测频率热更新 */
    if (now - s_lastCfg >= WEB_CFG_POLL_MS) {
        s_lastCfg = now;
        CfgSnap cs;
        int hz = constrain((int)cs.c.telemetryHz, 1, 50);
        s_tmPeriod = (uint16_t)(1000 / hz);
    }

    /* ② 遥测定时推送 */
    if (now - s_lastTm >= s_tmPeriod) {
        s_lastTm = now;
        if (s_ws.count() > 0) wsBroadcast(buildTelemetry());
    }

    /* ③ 回传事件（完成 / 标定 / 告警）—— 必须每帧泵，保证告警零延迟 */
    pumpDone();

    /* ④ 客户端句柄回收 */
    if (now - s_lastClean >= WEB_CLEANUP_MS) {
        s_lastClean = now;
        s_ws.cleanupClients();
    }
}
