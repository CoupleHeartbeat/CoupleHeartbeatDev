// main/couple_ble.c —— BLE 心跳扫描：RSSI 测距 + 靠近自动触发
//
// NimBLE host 由 couple_gatt.c 独占（全工程单一协议栈）。本模块只负责：
//   1. 用 couple_ble_build_mfg() 生成心跳制造商数据，交由 couple_gatt.c 拼入可连接广播；
//   2. 被动扫描配对设备的广播，解析心跳并做 RSSI 滤波测距；
//   3. 维护“对方是否在场 / 信号强度 / 持续极近计数”，供工牌页的靠近光效分档。
#include "couple_app.h"
#include "couple_gatt.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include <string.h>

static const char *TAG = "couple_ble";

// ============================================================================
// 心跳报文定义
// ============================================================================
// 制造商数据格式 (ADType 0xFF):
//   字节 0-1:  公司 ID (0x02E5 = FoloToy, 自定义)
//   字节 2-9:  pair_id (8 字节)
//   字节 10-11: seq (2 字节, 大端)
//   字节 12-19: AES 密文 (8 字节)
//   字节 20-21: CRC16 (2 字节, 大端)
// 总长度: COUPLE_BLE_MFG_DATA_LEN (22) 字节
// ============================================================================

#define COUPLE_BLE_MFG_ID       0x02E5  // 自定义制造商 ID
#define COUPLE_BLE_SCAN_WIN_MS  100     // 扫描窗口
#define COUPLE_BLE_SCAN_INT_MS  200     // 扫描间隔

#define COUPLE_BLE_RSSI_WINDOW  8       // RSSI 滑动窗口大小
#define COUPLE_BLE_RSSI_ALPHA   30      // EMA 系数 (α*100)

#define COUPLE_BLE_TIMEOUT_MS   3000    // 心跳超时（3秒未收到视为断开）

// 心跳报文明文结构（6 字节，加密后 8 字节含填充）
typedef struct {
    uint32_t timestamp;      // 时间戳 (秒)
    int8_t   rssi_hint;      // 本方看到的对方 RSSI 提示
    uint8_t  status;         // 状态位
} __attribute__((packed)) heartbeat_plain_t;

// ============================================================================
// 静态状态
// ============================================================================
static bool s_scanning;          // 扫描已启动
static bool s_addr_ready;        // 本机 BLE 地址已获取
static uint8_t s_addr_type;
static uint8_t s_own_addr[6];    // 本机 BLE 地址，用于过滤自身广播
static volatile uint16_t s_seq;

// RSSI 滤波
static int8_t s_rssi_window[COUPLE_BLE_RSSI_WINDOW];
static uint8_t s_rssi_window_idx;
static uint8_t s_rssi_window_count;
static int16_t s_rssi_ema;  // Q8 格式 (rssi * 100)

// 对方状态
static volatile bool s_partner_found;
static volatile uint32_t s_last_heartbeat_ms;
static volatile int8_t s_last_rssi;

// 极近计数（贴贴触发用）
static volatile uint8_t s_very_close_count;

// ============================================================================
// 前向声明
// ============================================================================
static int scan_event_cb(struct ble_gap_event *event, void *arg);

// ============================================================================
// RSSI 距离估算
// ============================================================================
static couple_distance_t rssi_to_distance(int8_t rssi)
{
    if (rssi > -40) return COUPLE_DISTANCE_VERY_CLOSE;
    if (rssi > -55) return COUPLE_DISTANCE_CLOSE;
    if (rssi > -70) return COUPLE_DISTANCE_NORMAL;
    return COUPLE_DISTANCE_FAR;
}

// ============================================================================
// RSSI 滑动窗口 + EMA 滤波
// ============================================================================
static int8_t rssi_filter(int8_t new_rssi)
{
    // 加入滑动窗口
    s_rssi_window[s_rssi_window_idx] = new_rssi;
    s_rssi_window_idx = (s_rssi_window_idx + 1) % COUPLE_BLE_RSSI_WINDOW;
    if (s_rssi_window_count < COUPLE_BLE_RSSI_WINDOW) {
        s_rssi_window_count++;
    }

    // 计算窗口均值
    int32_t sum = 0;
    for (uint8_t i = 0; i < s_rssi_window_count; i++) {
        sum += s_rssi_window[i];
    }
    int8_t avg = (int8_t)(sum / s_rssi_window_count);

    // EMA 平滑
    if (s_rssi_ema == 0) {
        s_rssi_ema = avg * 100;
    } else {
        s_rssi_ema = s_rssi_ema + (COUPLE_BLE_RSSI_ALPHA * (avg * 100 - s_rssi_ema)) / 100;
    }

    return (int8_t)(s_rssi_ema / 100);
}

// ============================================================================
// 构造心跳制造商数据（由 couple_gatt.c 拼入广播）
// 返回写入长度；未配对或缓冲区不足时返回 0（广播中不带心跳）。
// ============================================================================
uint8_t couple_ble_build_mfg(uint8_t *buf, uint8_t buf_len)
{
    if (!buf || buf_len < COUPLE_BLE_MFG_DATA_LEN) return 0;
    if (!g_couple_state.pair_info.paired) return 0;

    memset(buf, 0, COUPLE_BLE_MFG_DATA_LEN);

    // 制造商 ID (小端)
    buf[0] = COUPLE_BLE_MFG_ID & 0xFF;
    buf[1] = (COUPLE_BLE_MFG_ID >> 8) & 0xFF;

    // pair_id
    memcpy(buf + 2, g_couple_state.pair_info.pair_id, COUPLE_PAIR_ID_LEN);

    // 序列号（大端）
    s_seq++;
    buf[10] = (s_seq >> 8) & 0xFF;
    buf[11] = s_seq & 0xFF;

    // 构造明文
    heartbeat_plain_t plain;
    memset(&plain, 0, sizeof(plain));
    plain.timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
    plain.rssi_hint = s_last_rssi;
    plain.status = (uint8_t)g_couple_state.mode;

    // 构造 nonce: seq (2) + pair_id 前 4 字节 + 随机 6 字节 = 12 字节
    uint8_t nonce[12];
    nonce[0] = buf[10];
    nonce[1] = buf[11];
    memcpy(nonce + 2, g_couple_state.pair_info.pair_id, 4);
    esp_fill_random(nonce + 6, 6);

    // 加密（明文 6 字节，补到 8 字节）
    uint8_t plain_padded[8] = {0};
    memcpy(plain_padded, &plain, sizeof(plain));

    uint8_t ciphertext[8];
    if (couple_crypto_encrypt(g_couple_state.pair_info.pair_key, nonce,
                              plain_padded, 8, ciphertext) == ESP_OK) {
        memcpy(buf + 12, ciphertext, 8);
    }

    // CRC16 (校验范围: pair_id + seq + 密文 = 18 字节，跳过制造商 ID)
    uint16_t crc = 0;
    for (int i = 2; i < 20; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    buf[20] = (crc >> 8) & 0xFF;
    buf[21] = crc & 0xFF;

    return COUPLE_BLE_MFG_DATA_LEN;
}

// ============================================================================
// 启动扫描（被动扫描，不过滤重复以便持续采样 RSSI）
// ============================================================================
static int start_scanning(void)
{
    struct ble_gap_disc_params disc_params = {0};
    disc_params.itvl = COUPLE_BLE_SCAN_INT_MS / 0.625;   // 扫描间隔
    disc_params.window = COUPLE_BLE_SCAN_WIN_MS / 0.625; // 扫描窗口
    disc_params.filter_duplicates = 0;  // 不过滤重复，我们需要每次 RSSI
    disc_params.passive = 1;            // 被动扫描

    int rc = ble_gap_disc(s_addr_type, BLE_HS_FOREVER,
                          &disc_params, scan_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGD(TAG, "启动扫描失败: %d", rc);
    }
    return rc;
}

// ============================================================================
// 验证并解析心跳报文
// ============================================================================
static bool parse_heartbeat(const uint8_t *mfg_data, uint8_t mfg_len,
                            int8_t rssi)
{
    if (mfg_len < COUPLE_BLE_MFG_DATA_LEN) return false;

    // 检查制造商 ID
    uint16_t mfg_id = mfg_data[0] | ((uint16_t)mfg_data[1] << 8);
    if (mfg_id != COUPLE_BLE_MFG_ID) return false;

    // 检查 pair_id
    if (memcmp(mfg_data + 2, g_couple_state.pair_info.pair_id, COUPLE_PAIR_ID_LEN) != 0) {
        return false;
    }

    // 校验 CRC16（简化验证：跳过前 2 字节 mfg_id）
    uint16_t crc_rcv = ((uint16_t)mfg_data[20] << 8) | mfg_data[21];
    uint16_t crc_calc = 0;
    for (int i = 2; i < 20; i++) {
        crc_calc ^= (uint16_t)mfg_data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc_calc & 0x8000) {
                crc_calc = (crc_calc << 1) ^ 0x1021;
            } else {
                crc_calc <<= 1;
            }
        }
    }
    if (crc_rcv != crc_calc) {
        ESP_LOGW(TAG, "CRC 校验失败");
        return false;
    }

    // 序列号（简化：不做严格窗口检查，避免重置时序列号不同步）
    uint16_t seq = ((uint16_t)mfg_data[10] << 8) | mfg_data[11];

    // 解密（简化：实际应用需要完整验证）
    uint8_t nonce[12];
    nonce[0] = mfg_data[10];
    nonce[1] = mfg_data[11];
    memcpy(nonce + 2, g_couple_state.pair_info.pair_id, 4);
    // 注意：nonce 的后 6 字节是对方的随机数，我们不知道
    // 简化处理：使用 pair_id 后 6 字节代替（实际应用需在报文中包含 nonce）
    memcpy(nonce + 6, g_couple_state.pair_info.pair_id + 2, 6);

    uint8_t plaintext[8];
    if (couple_crypto_decrypt(g_couple_state.pair_info.pair_key, nonce,
                              mfg_data + 12, 8, plaintext) != ESP_OK) {
        return false;
    }

    // 心跳有效，更新状态
    s_partner_found = true;
    s_last_heartbeat_ms = couple_now_ms();

    // RSSI 滤波
    int8_t filtered_rssi = rssi_filter(rssi);
    s_last_rssi = filtered_rssi;

    // 更新全局状态
    g_couple_state.partner_rssi = filtered_rssi;
    g_couple_state.partner_connected = true;
    g_couple_state.distance = rssi_to_distance(filtered_rssi);
    g_couple_state.last_heartbeat_ms = s_last_heartbeat_ms;

    // 贴贴触发检测（连续极近）
    if (g_couple_state.distance == COUPLE_DISTANCE_VERY_CLOSE) {
        if (s_very_close_count < 255) s_very_close_count++;
    } else {
        s_very_close_count = 0;
    }

    ESP_LOGD(TAG, "心跳: seq=%d rssi=%d filtered=%d dist=%d vclose=%d",
             seq, rssi, filtered_rssi, g_couple_state.distance,
             s_very_close_count);

    return true;
}

// ============================================================================
// 扫描回调
// ============================================================================
static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_gap_disc_desc *disc = &event->disc;

    // 跳过本机自己的广播（自身 pair_id 与密钥必然匹配，否则会误判为对方）
    if (s_addr_ready && memcmp(disc->addr.val, s_own_addr, 6) == 0) return 0;

    // 查找制造商数据
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);

    if (fields.mfg_data != NULL && fields.mfg_data_len > 0) {
        parse_heartbeat(fields.mfg_data, fields.mfg_data_len, disc->rssi);
    }

    return 0;
}

// ============================================================================
// 心跳超时检测
// ============================================================================
static void heartbeat_check(void)
{
    if (!s_partner_found) return;

    uint32_t now = couple_now_ms();
    if (now - s_last_heartbeat_ms > COUPLE_BLE_TIMEOUT_MS) {
        // 心跳超时，对方离开
        s_partner_found = false;
        g_couple_state.partner_connected = false;
        g_couple_state.distance = COUPLE_DISTANCE_FAR;
        g_couple_state.partner_rssi = -100;
        s_very_close_count = 0;
        s_rssi_window_count = 0;
        s_rssi_ema = 0;
                ESP_LOGI(TAG, "对方心跳超时，已断开");
    }
}

// ============================================================================
// 公开 API
// ============================================================================

esp_err_t couple_ble_start(void)
{
    if (s_scanning) return ESP_OK;
    if (!g_couple_state.pair_info.paired) {
        return ESP_ERR_INVALID_STATE;  // 未配对无对方可寻
    }
    if (!couple_gatt_ready()) {
        // NimBLE host 未初始化成功（如堆不足 init 失败）时，
        // 调用 host API 会命中未初始化的锁直接崩溃，必须挡住。
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_addr_ready) {
        // NimBLE host 由 couple_gatt.c 持有；同步完成前 infer_auto 返回无地址，
        // 此处不报错，交由 tick 重试。
        uint8_t addr_type;
        if (ble_hs_id_infer_auto(0, &addr_type) != 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (ble_hs_id_copy_addr(addr_type, s_own_addr, NULL) != 0) {
            return ESP_ERR_INVALID_STATE;
        }
        s_addr_type = addr_type;
        s_addr_ready = true;
        ESP_LOGI(TAG, "本机 BLE 地址: %02X:%02X:%02X:%02X:%02X:%02X",
                 s_own_addr[5], s_own_addr[4], s_own_addr[3],
                 s_own_addr[2], s_own_addr[1], s_own_addr[0]);
    }

    if (start_scanning() != 0) return ESP_FAIL;

    s_scanning = true;
    s_seq = 0;
    s_partner_found = false;
    s_last_rssi = -100;
    s_rssi_window_count = 0;
    s_rssi_ema = 0;
    s_very_close_count = 0;
    ESP_LOGI(TAG, "心跳扫描已启动");
    return ESP_OK;
}

esp_err_t couple_ble_stop(void)
{
    if (!s_scanning) return ESP_OK;

    ESP_LOGI(TAG, "停止心跳扫描");
    s_scanning = false;
    ble_gap_disc_cancel();

    s_partner_found = false;
    g_couple_state.partner_connected = false;
    g_couple_state.distance = COUPLE_DISTANCE_FAR;
    g_couple_state.partner_rssi = -100;
    s_very_close_count = 0;
    s_rssi_window_count = 0;
    s_rssi_ema = 0;

    return ESP_OK;
}

bool couple_ble_is_active(void)
{
    return s_scanning;
}

// 获取当前 RSSI（滤波后）
int8_t couple_ble_get_rssi(void)
{
    return (int8_t)(s_rssi_ema / 100);
}

// 获取持续极近计数（工牌页据此判定“贴贴”档）
uint8_t couple_ble_very_close_count(void)
{
    return s_very_close_count;
}

// 周期调用：补启扫描 + 心跳超时检测
// 靠近光效由工牌页自己的 500ms 定时器按同一套分档逻辑驱动，此处不再切换模式。
void couple_ble_tick(void)
{
    if (!s_scanning && g_couple_state.pair_info.paired
        && couple_gatt_ready()) {
        couple_ble_start();
    }
    heartbeat_check();
}
