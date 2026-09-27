#include <cJSON.h>
#include "system_info.h"
#include <stdio.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "wifi_board.h"
#include "board_power_bsp.h"
#include "custom_lcd_display.h"
#include "lvgl.h"
#include "mcp_server.h"
#include "pixel_face.h"
#include <esp_timer.h>
#include "application.h"
#include "mcp_server.h"
#include <esp_random.h>
#include <string>
#include "assets/lang_config.h"
#include <esp_wifi.h>
#include "widget_art.h"
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_event.h>
#include <driver/rtc_io.h>
#include <freertos/event_groups.h>
#include <sys/time.h>
#include <cstring>
#include "ssid_manager.h"
#include <nvs.h>
#include <math.h>
#include <cmath>
#include <memory>
#define TAG "waveshare_epaper_1_54"

// ===== ЧАСОВОЙ ПОЯС =====
// Кипр: зимой UTC+2, летом UTC+3, переход автоматически.
static constexpr const char* kTimeZone = "EET-2EEST,M3.5.0/3,M10.5.0/4";

// ===== МИКРОФОН =====
// Усиление микрофона в дБ. У кодека ступени по 6 дБ: 0, 6, 12 ... 42.
// По умолчанию в xiaozhi 30. Каждые +6 дБ это примерно вдвое громче.
// Если он начнёт слышать сам себя или шипеть, верни на 30.
static constexpr float kMicGainDb = 30.0f;

static bool IsTimeValidNow() {
    return time(nullptr) > 1600000000;   // время уже пришло из сети
}

// Точное время из интернета (SNTP) уже пришло хотя бы раз.
// Только ему можно верить: сервер xiaozhi при проверке обновлений ставит
// часы сразу по местному времени, и вместе с часовым поясом выходит +3 часа.
static volatile bool s_sntp_synced = false;

static void OnSntpSync(struct timeval* tv) {
    s_sntp_synced = true;
    ESP_LOGI("waveshare_epaper_1_54", "SNTP: time synced");
}

// ===== РЕЖИМ ЧАСОВ (deep sleep) =====
// Настройки. Меняй смело, остальной код подстроится.
static constexpr int kDeskWakeSec = 60;                    // как часто просыпаться и обновлять экран
static constexpr int64_t kDeskSyncSec = 24LL * 60 * 60;    // как часто ходить в сеть за временем и погодой
static constexpr int kDeskFullRefreshEvery = 60;           // раз в столько обновлений полная перерисовка (чистит следы)
static constexpr int kDeskLongPressMs = 1200;              // удержание PWR во сне = выключение
static constexpr int kDeskWifiTimeoutMs = 12000;           // сколько ждать Wi-Fi на одну сеть
static constexpr uint32_t kDeskMagic = 0x4B4F5242;         // метка "данные режима часов настоящие"
static constexpr int kDeskFrameBytes = 5000;               // размер кадра экрана 200x200 по 1 биту
static constexpr bool kDeskPageBarAlways = false;          // true: полоска страниц видна всегда, а не только при листании

// ===== ВИДЖЕТЫ ПО ГОЛОСУ =====
static constexpr int kVoiceWidgetSec = 15;   // сколько секунд виджет на экране во время разговора

// ===== АВТОСОН =====
// Если столько минут подряд никто не разговаривает, устройство само
// уходит в режим часов (как по кнопке PWR). 0 = никогда не засыпать.
// Не засыпает, пока идёт таймер или секундомер.
static constexpr int kAutoDeskAfterMin = 5;

// Погода в простом виде, который переживает глубокий сон.
struct DeskRtcWeather {
    bool valid;
    float temperature;
    int16_t code;
    int16_t humidity;
    bool has_days;
    int8_t day[3];
    int16_t day_code[3];
    float day_tmax[3];
    int64_t fetched_unix;
};

// Всё, что режим часов помнит между пробуждениями. Лежит в RTC-памяти,
// она не стирается во сне. При обычном включении обнуляется.
struct DeskRtcState {
    uint32_t magic;
    bool active;                  // сейчас режим часов
    uint8_t page;                 // какая страница на экране
    uint16_t updates_since_full;  // сколько быстрых обновлений с последнего полного
    int64_t last_sync;            // когда последний раз ходили в сеть
    DeskRtcWeather weather;
    bool frame_valid;
    uint8_t frame[kDeskFrameBytes];   // что сейчас нарисовано на экране
};

static RTC_DATA_ATTR DeskRtcState s_desk;

// ===== ИСТОРИЯ ДЛЯ ГРАФИКОВ =====
// Комната раз в 10 минут за сутки, батарея раз в час за неделю.
// Пишется и в обычном режиме, и в режиме часов. Живёт в RTC-памяти
// (переживает сон), а раз в час сохраняется во флеш (переживает выключение).
static constexpr uint32_t kHistMagic = 0x48495354;   // "HIST"
static constexpr uint32_t kFcMagic = 0x46434153;     // "FCAS"
static constexpr int16_t kHistNoTemp = -32768;
static constexpr uint8_t kHistNoValue = 255;

struct HistRtc {
    uint32_t magic;
    int64_t room_last_slot;                  // номер 10-минутки последней записи
    int16_t room_t10[DESK_ROOM_POINTS];      // температура x10
    uint8_t room_h[DESK_ROOM_POINTS];        // влажность, %
    int64_t batt_last_slot;                  // номер часа последней записи
    uint8_t batt[DESK_BATT_POINTS];          // заряд, %
};

static RTC_DATA_ATTR HistRtc s_hist;
static RTC_DATA_ATTR ForecastExtra s_fc;

static void HistClear() {
    memset(&s_hist, 0, sizeof(s_hist));
    s_hist.magic = kHistMagic;
    for (int i = 0; i < DESK_ROOM_POINTS; i++) {
        s_hist.room_t10[i] = kHistNoTemp;
        s_hist.room_h[i] = kHistNoValue;
    }
    for (int i = 0; i < DESK_BATT_POINTS; i++) {
        s_hist.batt[i] = kHistNoValue;
    }
}

static void HistSaveToFlash() {
    nvs_handle_t h;
    if (nvs_open("desk", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "hist", &s_hist, sizeof(s_hist));
    nvs_commit(h);
    nvs_close(h);
}

// После обычного включения RTC-память пустая: достаём историю из флеша.
static void HistEnsureLoaded() {
    if (s_hist.magic == kHistMagic) return;
    HistClear();
    nvs_handle_t h;
    if (nvs_open("desk", NVS_READONLY, &h) != ESP_OK) return;
    HistRtc tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, "hist", &tmp, &len) == ESP_OK && len == sizeof(tmp) &&
        tmp.magic == kHistMagic) {
        s_hist = tmp;
        ESP_LOGI("waveshare_epaper_1_54", "History loaded from flash");
    }
    nvs_close(h);
}

// Кладёт значение в кольцевой буфер по номеру слота. Пропущенные слоты
// помечаются "нет данных". Если время прыгнуло назад, история начинается заново.
template <typename T>
static void HistPut(T* ring, int size, int64_t& last_slot, int64_t slot, T value, T none) {
    if (last_slot == 0 || slot < last_slot || slot - last_slot >= size) {
        for (int i = 0; i < size; i++) ring[i] = none;
    } else {
        for (int64_t k = last_slot + 1; k < slot; k++) ring[k % size] = none;
    }
    ring[slot % size] = value;
    last_slot = slot;
}

// Записывает текущие показания, если начался новый слот.
static void HistRecord(bool room_ok, float t, float h, int battery) {
    if (time(nullptr) < 1600000000) return;   // без настоящего времени не пишем
    HistEnsureLoaded();
    int64_t now = (int64_t)time(nullptr);

    int64_t slot = now / 600;
    if (slot != s_hist.room_last_slot) {
        int16_t tv = room_ok ? (int16_t)lroundf(t * 10.0f) : kHistNoTemp;
        int hv = (int)lroundf(h);
        uint8_t hb = room_ok ? (uint8_t)(hv < 0 ? 0 : (hv > 100 ? 100 : hv)) : kHistNoValue;
        int64_t last_t = s_hist.room_last_slot;
        HistPut<int16_t>(s_hist.room_t10, DESK_ROOM_POINTS, last_t, slot, tv, kHistNoTemp);
        int64_t last_h = s_hist.room_last_slot;
        HistPut<uint8_t>(s_hist.room_h, DESK_ROOM_POINTS, last_h, slot, hb, kHistNoValue);
        s_hist.room_last_slot = slot;
    }

    int64_t hour = now / 3600;
    if (hour != s_hist.batt_last_slot) {
        uint8_t bv = (battery >= 0 && battery <= 100) ? (uint8_t)battery : kHistNoValue;
        HistPut<uint8_t>(s_hist.batt, DESK_BATT_POINTS, s_hist.batt_last_slot, hour, bv, kHistNoValue);
        HistSaveToFlash();   // раз в час во флеш
    }
}

// Примерно сколько дней проживёт батарея: скорость разряда с последней зарядки.
static int EstimateBatteryDays() {
    if (s_hist.magic != kHistMagic || s_hist.batt_last_slot == 0) return -1;
    int64_t last = s_hist.batt_last_slot;
    uint8_t v_now = s_hist.batt[last % DESK_BATT_POINTS];
    if (v_now == kHistNoValue) return -1;
    int v_start = v_now;
    int hours = 0;
    for (int k = 1; k < DESK_BATT_POINTS; k++) {
        uint8_t v = s_hist.batt[(last - k) % DESK_BATT_POINTS];
        if (v == kHistNoValue) break;
        if (v + 2 < v_start) break;   // раньше заряд был ниже: тут заряжали
        if (v > v_start) v_start = v;
        hours = k;
    }
    int drop = v_start - v_now;
    if (hours < 6 || drop < 3) return -1;   // мало данных для оценки
    float per_hour = (float)drop / (float)hours;
    int days = (int)lroundf((float)v_now / per_hour / 24.0f);
    return days > 99 ? 99 : days;
}

// Раскладывает историю и прогноз в массивы для графиков.
static void FillChartData(DeskScreenData& d) {
    d.now = (int64_t)time(nullptr);
    if (d.now < 1600000000) return;

    if (s_hist.magic == kHistMagic) {
        int64_t slot_now = d.now / 600;
        d.room_t0 = (slot_now - (DESK_ROOM_POINTS - 1)) * 600;
        for (int i = 0; i < DESK_ROOM_POINTS; i++) {
            int64_t slot = slot_now - (DESK_ROOM_POINTS - 1) + i;
            if (slot > s_hist.room_last_slot || slot <= s_hist.room_last_slot - DESK_ROOM_POINTS) continue;
            int16_t tv = s_hist.room_t10[slot % DESK_ROOM_POINTS];
            uint8_t hv = s_hist.room_h[slot % DESK_ROOM_POINTS];
            if (tv != kHistNoTemp) d.room_t_hist[i] = tv / 10.0f;
            if (hv != kHistNoValue) d.room_h_hist[i] = (float)hv;
        }
        int64_t hour_now = d.now / 3600;
        d.batt_t0 = (hour_now - (DESK_BATT_POINTS - 1)) * 3600;
        for (int i = 0; i < DESK_BATT_POINTS; i++) {
            int64_t slot = hour_now - (DESK_BATT_POINTS - 1) + i;
            if (slot > s_hist.batt_last_slot || slot <= s_hist.batt_last_slot - DESK_BATT_POINTS) continue;
            uint8_t bv = s_hist.batt[slot % DESK_BATT_POINTS];
            if (bv != kHistNoValue) d.batt_hist[i] = (float)bv;
        }
        d.batt_days_left = EstimateBatteryDays();
    }

    d.hour0 = d.now - d.now % 3600;
    if (s_fc.magic == kFcMagic) {
        for (int i = 0; i < 25; i++) {
            int64_t tp = d.hour0 - (int64_t)(24 - i) * 3600;
            int64_t ip = (tp - s_fc.hourly_start) / 3600;
            if (ip >= 0 && ip < FC_HOURS && s_fc.hourly_t10[ip] != FC_NO_TEMP) {
                d.out_past[i] = s_fc.hourly_t10[ip] / 10.0f;
            }
            int64_t tn = d.hour0 + (int64_t)i * 3600;
            int64_t in = (tn - s_fc.hourly_start) / 3600;
            if (in >= 0 && in < FC_HOURS && s_fc.hourly_t10[in] != FC_NO_TEMP) {
                d.out_next[i] = s_fc.hourly_t10[in] / 10.0f;
            }
            if (i < 24 && in >= 0 && in < FC_HOURS && s_fc.hourly_rain[in] != FC_NO_VALUE) {
                d.rain_next[i] = (float)s_fc.hourly_rain[in];
            }
        }
        for (int i = 0; i < FC_DAYS; i++) {
            if (s_fc.day_start[i] <= d.now && d.now < s_fc.day_start[i] + 86400 &&
                s_fc.sunrise[i] > 0 && s_fc.sunset[i] > s_fc.sunrise[i]) {
                d.sun_ok = true;
                d.sunrise = s_fc.sunrise[i];
                d.sunset = s_fc.sunset[i];
                d.uv_max = (s_fc.uv10[i] != FC_NO_VALUE) ? s_fc.uv10[i] / 10.0f : -1.0f;
                break;
            }
        }
    }
}

// Для ожидания подключения к Wi-Fi во сне.
static EventGroupHandle_t s_desk_wifi_events = nullptr;
static constexpr EventBits_t kDeskWifiGotIp = BIT0;
static constexpr EventBits_t kDeskWifiLost = BIT1;

static void DeskWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (s_desk_wifi_events == nullptr) return;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_desk_wifi_events, kDeskWifiGotIp);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_desk_wifi_events, kDeskWifiLost);
    }
}

// ===== ПОГОДА (Open-Meteo, Лимассол, Кипр) =====
// Вставить после блока с монеткой (self.game.coin_flip)
 
// Координаты Лимассола, Кипр
static constexpr double kWeatherLat = 34.7071;
static constexpr double kWeatherLon = 33.0226;

static WeatherCache s_weather_cache;
static constexpr int64_t kWeatherCacheTtlMs = 10 * 60 * 1000; // 10 минут
 
// Переводит weather_code Open-Meteo (WMO) в человекочитаемое описание
static std::string WeatherCodeToText(int code) {
    if (code == 0) return "ясно";
    if (code == 1 || code == 2) return "малооблачно";
    if (code == 3) return "пасмурно";
    if (code == 45 || code == 48) return "туман";
    if (code >= 51 && code <= 57) return "морось";
    if (code >= 61 && code <= 67) return "дождь";
    if (code >= 71 && code <= 77) return "снег";
    if (code >= 80 && code <= 82) return "ливень";
    if (code >= 85 && code <= 86) return "снегопад";
    if (code >= 95 && code <= 99) return "гроза";
    return "неизвестно";
}

// Готовая фраза для голоса.
static std::string MakeWeatherSummary(float temperature, int weather_code) {
    char summary[128];
    snprintf(summary, sizeof(summary),
             "В Лимассоле сейчас %s, температура %.0f градусов",
             WeatherCodeToText(weather_code).c_str(), temperature);
    return summary;
}
 
// Делает запрос к Open-Meteo и обновляет кэш.
// Возвращает true при успехе. network: чем ходить в интернет
// (в обычном режиме и в режиме часов это разные пути).
template <typename Net>
static bool FetchWeather(Net* network) {
    // Время в ответе в виде unix-секунд, так проще и нет путаницы с поясами.
    // past_days=1 даёт ещё и вчерашний день: нужен графику "дом и улица за сутки".
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,relative_humidity_2m"
             "&hourly=temperature_2m,precipitation_probability"
             "&daily=weather_code,temperature_2m_max,sunrise,sunset,uv_index_max"
             "&past_days=1&forecast_days=4&timezone=auto&timeformat=unixtime",
             kWeatherLat, kWeatherLon);
 
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE("Weather", "Failed to create http client");
        return false;
    }
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
 
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGE("Weather", "Failed to open HTTP connection: %s", opened.error().ToString().c_str());
        return false;
    }
 
    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE("Weather", "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        http->Close();
        return false;
    }
    if (*status_code != 200) {
        ESP_LOGE("Weather", "Open-Meteo returned status code: %d", *status_code);
        http->Close();
        return false;
    }
 
    std::string body = http->ReadAll();
    http->Close();
 
    if (body.empty()) {
        ESP_LOGE("Weather", "Empty response from Open-Meteo");
        return false;
    }
 
    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE("Weather", "Failed to parse JSON");
        return false;
    }
 
    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (current == nullptr) {
        ESP_LOGE("Weather", "No 'current' field in response");
        cJSON_Delete(root);
        return false;
    }
 
    cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON *code = cJSON_GetObjectItem(current, "weather_code");
    if (temp == nullptr || code == nullptr) {
        cJSON_Delete(root);
        return false;
    }
 
    float temperature = (float)temp->valuedouble;
    int weather_code = code->valueint;

    // влажность
    cJSON *hum = cJSON_GetObjectItem(current, "relative_humidity_2m");
    int humidity = (hum != nullptr) ? hum->valueint : 0;

    // Точка отсчёта "сейчас": часы устройства, а если они ещё не
    // выставлены, время из ответа сервера.
    int64_t ref_now = (int64_t)time(nullptr);
    cJSON *ctime_item = cJSON_GetObjectItem(current, "time");
    if (!IsTimeValidNow() && ctime_item != nullptr && cJSON_IsNumber(ctime_item)) {
        ref_now = (int64_t)ctime_item->valuedouble;
    }

    // Дни: [0] вчера, [1] сегодня, [2..4] следующие три дня.
    WeatherDay days[3];
    bool has_days = false;
    bool have_fc = false;
    ForecastExtra fc;
    memset(&fc, 0, sizeof(fc));
    for (int i = 0; i < FC_HOURS; i++) {
        fc.hourly_t10[i] = FC_NO_TEMP;
        fc.hourly_rain[i] = FC_NO_VALUE;
    }
    for (int i = 0; i < FC_DAYS; i++) {
        fc.uv10[i] = FC_NO_VALUE;
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily != nullptr) {
        cJSON *dtime = cJSON_GetObjectItem(daily, "time");
        cJSON *dcode = cJSON_GetObjectItem(daily, "weather_code");
        cJSON *dmax  = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *drise = cJSON_GetObjectItem(daily, "sunrise");
        cJSON *dset  = cJSON_GetObjectItem(daily, "sunset");
        cJSON *duv   = cJSON_GetObjectItem(daily, "uv_index_max");
        if (cJSON_IsArray(dtime) && cJSON_IsArray(dcode) && cJSON_IsArray(dmax) &&
            cJSON_GetArraySize(dtime) >= 5) {
            has_days = true;
            for (int i = 0; i < 3; i++) {
                cJSON *ts = cJSON_GetArrayItem(dtime, i + 2);
                cJSON *cs = cJSON_GetArrayItem(dcode, i + 2);
                cJSON *ms = cJSON_GetArrayItem(dmax,  i + 2);
                if (ts == nullptr || cs == nullptr || ms == nullptr) {
                    has_days = false;
                    break;
                }
                time_t day_t = (time_t)ts->valuedouble;
                struct tm tm_day;
                localtime_r(&day_t, &tm_day);
                days[i].day  = tm_day.tm_mday;
                days[i].code = cs->valueint;
                days[i].tmax = (float)ms->valuedouble;
            }
        }
        // восход, закат, УФ по дням
        if (cJSON_IsArray(dtime) && cJSON_IsArray(drise) && cJSON_IsArray(dset)) {
            int n = cJSON_GetArraySize(dtime);
            if (n > FC_DAYS) n = FC_DAYS;
            for (int i = 0; i < n; i++) {
                cJSON *t = cJSON_GetArrayItem(dtime, i);
                cJSON *r = cJSON_GetArrayItem(drise, i);
                cJSON *z = cJSON_GetArrayItem(dset, i);
                if (t != nullptr && cJSON_IsNumber(t)) fc.day_start[i] = (int64_t)t->valuedouble;
                if (r != nullptr && cJSON_IsNumber(r)) fc.sunrise[i] = (int64_t)r->valuedouble;
                if (z != nullptr && cJSON_IsNumber(z)) fc.sunset[i] = (int64_t)z->valuedouble;
                cJSON *u = cJSON_IsArray(duv) ? cJSON_GetArrayItem(duv, i) : nullptr;
                if (u != nullptr && cJSON_IsNumber(u)) {
                    int uv10 = (int)(u->valuedouble * 10.0 + 0.5);
                    fc.uv10[i] = (uint8_t)(uv10 > 254 ? 254 : (uv10 < 0 ? 0 : uv10));
                }
            }
            have_fc = true;
        }
    }

    // По часам: сутки назад и трое суток вперёд от текущего часа.
    cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (hourly != nullptr) {
        cJSON *htime = cJSON_GetObjectItem(hourly, "time");
        cJSON *htemp = cJSON_GetObjectItem(hourly, "temperature_2m");
        cJSON *hrain = cJSON_GetObjectItem(hourly, "precipitation_probability");
        if (cJSON_IsArray(htime) && cJSON_IsArray(htemp)) {
            int64_t start = (ref_now - ref_now % 3600) - 24LL * 3600;
            fc.hourly_start = start;
            int n = cJSON_GetArraySize(htime);
            for (int i = 0; i < n; i++) {
                cJSON *t = cJSON_GetArrayItem(htime, i);
                if (t == nullptr || !cJSON_IsNumber(t)) continue;
                int64_t idx = ((int64_t)t->valuedouble - start) / 3600;
                if (idx < 0 || idx >= FC_HOURS) continue;
                cJSON *v = cJSON_GetArrayItem(htemp, i);
                if (v != nullptr && cJSON_IsNumber(v)) {
                    fc.hourly_t10[idx] = (int16_t)lround(v->valuedouble * 10.0);
                }
                cJSON *r = cJSON_IsArray(hrain) ? cJSON_GetArrayItem(hrain, i) : nullptr;
                if (r != nullptr && cJSON_IsNumber(r)) {
                    int p = r->valueint;
                    fc.hourly_rain[idx] = (uint8_t)(p < 0 ? 0 : (p > 100 ? 100 : p));
                }
            }
            have_fc = true;
        }
    }
    if (have_fc) {
        fc.magic = kFcMagic;
        s_fc = fc;
    }

    s_weather_cache.summary = MakeWeatherSummary(temperature, weather_code);
    s_weather_cache.temperature = temperature;
    s_weather_cache.weather_code = weather_code;
    s_weather_cache.humidity = humidity;
    s_weather_cache.has_days = has_days;
    for (int i = 0; i < 3; i++) {
        s_weather_cache.days[i] = days[i];
    }
    s_weather_cache.fetched_at_ms = esp_timer_get_time() / 1000;
    s_weather_cache.fetched_at_unix = IsTimeValidNow() ? (int64_t)time(nullptr) : 0;
    s_weather_cache.valid = true;

    cJSON_Delete(root);
    return true;
}
 
// Возвращает актуальные данные о погоде, обновляя кэш при необходимости.
// Используется и голосовым MCP tool, и заставкой на экране (этап 2, п.2).
static bool GetWeather(WeatherCache &out) {
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (!s_weather_cache.valid ||
        (now_ms - s_weather_cache.fetched_at_ms) > kWeatherCacheTtlMs) {
        if (!FetchWeather(Board::GetInstance().GetNetwork()) && !s_weather_cache.valid) {
            return false; // не было ни разу удачного запроса
        }
    }
    out = s_weather_cache;
    return true;
}

// Перекладывание погоды туда и обратно для RTC-памяти.
static DeskRtcWeather WeatherToRtc(const WeatherCache& w) {
    DeskRtcWeather r;
    memset(&r, 0, sizeof(r));
    r.valid = w.valid;
    r.temperature = w.temperature;
    r.code = (int16_t)w.weather_code;
    r.humidity = (int16_t)w.humidity;
    r.has_days = w.has_days;
    for (int i = 0; i < 3; i++) {
        r.day[i] = (int8_t)w.days[i].day;
        r.day_code[i] = (int16_t)w.days[i].code;
        r.day_tmax[i] = w.days[i].tmax;
    }
    r.fetched_unix = w.fetched_at_unix;
    return r;
}

static WeatherCache RtcToWeather(const DeskRtcWeather& r) {
    WeatherCache w;
    w.valid = r.valid;
    w.temperature = r.temperature;
    w.weather_code = r.code;
    w.humidity = r.humidity;
    w.has_days = r.has_days;
    for (int i = 0; i < 3; i++) {
        w.days[i].day = r.day[i];
        w.days[i].code = r.day_code[i];
        w.days[i].tmax = r.day_tmax[i];
    }
    w.fetched_at_unix = r.fetched_unix;
    if (w.valid) {
        w.summary = MakeWeatherSummary(w.temperature, w.weather_code);
    }
    return w;
}

class CustomBoard : public WifiBoard {
  private:
    i2c_master_bus_handle_t   i2c_bus_;
    Button                    boot_button_;
    Button                    pwr_button_;
    CustomLcdDisplay         *display_;
    BoardPowerBsp            *power_;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t         cali_handle;
    esp_timer_handle_t timer_handle_ = nullptr;
	esp_timer_handle_t alarm_handle_ = nullptr;
	int alarm_count_ = 0;
    int saved_volume_ = 0;
    int64_t timer_end_us_ = 0;
    int64_t stopwatch_start_us_ = 0;
    bool stopwatch_running_ = false;
	esp_timer_handle_t autolisten_timer_ = nullptr;
    int autolisten_retries_ = 0;
	esp_timer_handle_t led_timer_ = nullptr;
    bool led_blink_on_ = false;
	int idle_blink_tick_ = 0;
	esp_timer_handle_t boot_timer_ = nullptr;
    int boot_frame_ = 0;
	int wifi_check_tick_ = 0;
    int wifi_warn_cooldown_ = 0;
    bool desk_entering_ = false;       // уже переходим в режим часов
    bool desk_wait_speech_ = false;    // перед сном дождаться конца речи
    bool sntp_started_ = false;        // точное время из интернета уже запрошено
	
    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port          = (i2c_port_t) 0;
        i2c_bus_cfg.sda_io_num        = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num        = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority     = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
		
	}
	//кнопки
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
		
        pwr_button_.OnLongPress([this]() {
        display_->SetShuttingDown(true);
        display_->DrawFace(FACE_SAD);
        vTaskDelay(pdMS_TO_TICKS(1500));
        display_->DrawFace(FACE_DEAD);
        vTaskDelay(pdMS_TO_TICKS(1500));
        power_->PowerAudioOff();
        power_->PowerEpdOff();
        power_->VbatPowerOff();
    });

        // Короткое нажатие PWR: уйти в режим часов.
        pwr_button_.OnClick([this]() {
            // Первые секунды после включения нажатия не считаем: это
            // отпускание той самой кнопки, которой устройство включили.
            if (esp_timer_get_time() < 8000000LL) return;
            RequestDeskMode(false);
        });
    }

    void UpdateListenLed() {
        if (power_ == nullptr) {
            return;
        }
 
        auto& app = Application::GetInstance();
	
 
        if (app.GetDeviceState() == kDeviceStateListening) {
            idle_blink_tick_ = 0; // сбрасываем счётчик "сердцебиения"
            led_blink_on_ = !led_blink_on_;
            if (led_blink_on_) {
                power_->LedOn();
            } else {
                power_->LedOff();
            }
            return;
        }
 
        // Не слушаем. Если светодиод остался включённым после мигания
        // прослушки, гасим его один раз.
        if (led_blink_on_) {
            led_blink_on_ = false;
            power_->LedOff();
            idle_blink_tick_ = 0;
            return;
        }
 
        // "Сердцебиение": таймер тикает каждые 400 мс, значит 50 тиков
        // это 20 секунд. На 50-м тике коротко мигаем (один тик, 400 мс)
        // и сбрасываем счётчик.
        idle_blink_tick_++;
        if (idle_blink_tick_ >= 50) {
            idle_blink_tick_ = 0;
            power_->LedOn();
        } else if (idle_blink_tick_ == 1) {
            // Тик сразу после вспышки — гасим её, вспышка длится ровно
            // один период таймера, 400 мс.
            power_->LedOff();
        }
    }
 
    // Следит за уровнем сигнала Wi-Fi. Если он слабый, ненадолго
    // показывает лицо с иконкой сигнала, но только в простое и не
    // чаще раза в пять минут, чтобы не надоедать.
   void CheckWifiSignal() {
        // Точное время из интернета. Запускаем только когда устройство
        // готово (после проверки обновлений), иначе сервер перезапишет
        // точное время своим, уже сдвинутым на часовой пояс.
        if (!sntp_started_ &&
            Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            StartSntpOnce();
        }

        if (wifi_warn_cooldown_ > 0) wifi_warn_cooldown_--;

        

        // проверяем раз в тридцать секунд
        wifi_check_tick_++;
        if (wifi_check_tick_ < 75) return;
        wifi_check_tick_ = 0;

        // только в простое, чтобы не лезть посреди разговора
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) return;
        if (display_->GetFaceMode() != FaceMode::kIdle) return;
        if (wifi_warn_cooldown_ > 0) return;

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
        ESP_LOGI(TAG, "WiFi RSSI: %d", ap.rssi);
        if (ap.rssi > -80) return;

        display_->PlayWifiWeak();    // мигающий значок покажет аниматор лица
        wifi_warn_cooldown_ = 750;   // не чаще раза в пять минут
    }
	
	
    // История для графиков в обычном режиме: раз в минуту проверяем,
    // не пора ли записать новую точку (запись раз в 10 минут и раз в час).
    int history_tick_ = 140;   // первая проверка через несколько секунд после старта
    void HistoryTick() {
        history_tick_++;
        if (history_tick_ < 150) return;   // 150 тиков по 400 мс = минута
        history_tick_ = 0;
        if (time(nullptr) < 1600000000) return;
        int64_t now = (int64_t)time(nullptr);
        if (now / 600 == s_hist.room_last_slot && now / 3600 == s_hist.batt_last_slot) return;
        float t = 0, h = 0;
        bool ok = ReadSHTC3(t, h);
        HistRecord(ok, t, h, (int)BatterygetPercent());
    }

    // Автосон: считаем, сколько устройство подряд простаивает без
    // разговора. Любое общение, таймер или секундомер сбрасывают отсчёт.
    int64_t idle_since_us_ = 0;
    void AutoDeskTick() {
        if (kAutoDeskAfterMin <= 0 || desk_entering_) return;
        auto& app = Application::GetInstance();
        int64_t now = esp_timer_get_time();
        bool timer_busy = timer_end_us_ != 0 && now < timer_end_us_ + 60LL * 1000000;   // минута на будильник
        if (app.GetDeviceState() != kDeviceStateIdle || timer_busy || stopwatch_running_) {
            idle_since_us_ = 0;
            return;
        }
        if (idle_since_us_ == 0) {
            idle_since_us_ = now;
            return;
        }
        if (now - idle_since_us_ >= (int64_t)kAutoDeskAfterMin * 60 * 1000000) {
            ESP_LOGI(TAG, "Auto sleep: idle for %d min, entering desk mode", kAutoDeskAfterMin);
            idle_since_us_ = 0;
            RequestDeskMode(false);
        }
    }

    // Запускает таймер мигания. Период 400 мс даёт спокойное,
    // заметное мигание. Меньше значение, быстрее моргает.
    void StartListenLed() {
        esp_timer_create_args_t args = {};
          args.callback = [](void* arg) {
            auto* self = static_cast<CustomBoard*>(arg);
            self->UpdateListenLed();
            self->CheckWifiSignal();
            self->HistoryTick();
            self->AutoDeskTick();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "listen_led";
        args.skip_unhandled_events = true;
 
        if (esp_timer_create(&args, &led_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Listen LED: failed to create timer");
            return;
        }
        esp_timer_start_periodic(led_timer_, 400 * 1000);
    }
	
       
       
	   void InitializeTools() {
		    auto &mcp_server = McpServer::GetInstance();
			
//wifi
        mcp_server.AddTool("self.disp.network", "Reconfigure WiFi connection", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            EnterWifiConfigMode();
            return true;
        });
				

// монетка
        mcp_server.AddTool(
            "self.game.coin_flip",
            "Flip a coin (heads or tails, орёл или решка). "
            "Call this every time the user asks to flip a coin, toss a coin, "
            "or play heads or tails. Returns 'heads' or 'tails'. "
            "Announce the result to the user in their language.",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(2000));
                bool heads = (esp_random() & 1) != 0;
                return std::string(heads ? "heads" : "tails");
            });
			
// погода
		mcp_server.AddTool(
    "self.weather.get_current",
    "Get current weather in Limassol, Cyprus (Лимассол, Кипр). "
    "Call this every time the user asks about weather, temperature, "
    "what's it like outside, погода, температура на улице. "
    "Returns a short description of current conditions and temperature. "
    "Announce the result to the user in their language. "
    "The device shows the result on its small screen for a few "
    "seconds, then the screen goes back to the face. Call this tool every "
    "time the user asks, including when they ask to show it again. "
    "Never say that it is already on the screen. ",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                WeatherCache w;
                if (!GetWeather(w)) {
                    return std::string("Не удалось получить данные о погоде, попробуйте позже");
                }
                display_->ShowWeatherWidget(w, kVoiceWidgetSec);
                return w.summary;
            });
			
//температура и влажность на датчике
        mcp_server.AddTool(
            "self.sensor.get_room_climate",
            "Get current room temperature in Celsius and humidity in percent from the built-in sensor. "
            "Call this every time the user asks about the room, home temperature "
            "or humidity, температура дома, в комнате, влажность. "
            "The device shows these values on its small screen for a few "
            "seconds, then the screen goes back to the face. Call this tool every "
            "time the user asks, including when they ask to show it again. "
            "Never say that it is already on the screen. ",
            PropertyList(), [this](const PropertyList &) -> ReturnValue {
                float t = 0, h = 0;
                if (!ReadSHTC3(t, h)) {
                    return std::string("{\"success\":false}");
                }
                display_->ShowRoomWidget(t, h, kVoiceWidgetSec);
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"temperature\":%.1f,\"humidity\":%.1f}", t, h);
                return std::string(buf);
            });
			       
// уровень заряда (и график батареи на экране)
        mcp_server.AddTool(
            "self.battery.get_level",
            "Get the current battery charge level in percent and an estimate of days left. "
            "Call this when the user asks about the battery, the charge, "
            "how much power is left, заряд, батарея, сколько осталось заряда, "
            "на сколько хватит. "
            "The device shows the battery chart for the week on its screen for a few seconds. "
            "Tell the user the number in their language, briefly.",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                auto dp = std::make_unique<DeskScreenData>();
                CollectLiveData(*dp, DESK_PAGE_BATTERY);
                ESP_LOGI(TAG, "Battery: %d%%", dp->battery);
                display_->ShowDeskWidget(DESK_PAGE_BATTERY, *dp, kVoiceWidgetSec);
                return DescribePage(DESK_PAGE_BATTERY, *dp);
            });

// любой экран режима часов по голосу
        mcp_server.AddTool(
            "self.screen.show",
            "Show an information screen on the device display for a few seconds "
            "and get the data shown on it. Use it in the middle of a conversation too, "
            "every time the user asks about one of these topics or asks to show it. "
            "page is one of: "
            "clock (current time, date, day of week; время, который час, дата, какой день), "
            "forecast (outside temperature and chance of rain for the next 24 hours; "
            "прогноз на сутки, будет ли дождь, зонт), "
            "room_history (room temperature and humidity chart for the last 24 hours; "
            "график температуры дома, как менялась влажность), "
            "ventilate (home vs outside temperature for 24 hours, when to open the windows; "
            "проветрить, открыть окно, где теплее, дома или на улице), "
            "sun (sunrise, sunset, day length, UV index; восход, закат, солнце, ультрафиолет), "
            "moon (moon phase; луна, фаза луны, полнолуние), "
            "battery (battery chart for the week and days left). "
            "For the current weather prefer self.weather.get_current, for the room right now "
            "prefer self.sensor.get_room_climate. "
            "Tell the user the result briefly in their language. "
            "Never say that it is already on the screen.",
            PropertyList({Property("page", kPropertyTypeString)}),
            [this](const PropertyList &properties) -> ReturnValue {
                std::string name = properties["page"].value<std::string>();
                int page = PageFromName(name);
                if (page < 0) {
                    return std::string("{\"success\":false,\"error\":\"unknown page, use one of: "
                                       "clock, forecast, room_history, ventilate, sun, moon, battery\"}");
                }
                auto dp = std::make_unique<DeskScreenData>();
                CollectLiveData(*dp, page);
                display_->ShowDeskWidget(page, *dp, kVoiceWidgetSec);
                return DescribePage(page, *dp);
            });
			//бросить кубик
			mcp_server.AddTool(
    "self.game.dice",
    "Roll dice. Use when the user asks to roll a die or dice. "
    "sides is the number of sides (default 6), count is how many dice (default 1). "
    "Announce the result in the user's language.",
    PropertyList({
        Property("sides", kPropertyTypeInteger, 6, 2, 100),
        Property("count", kPropertyTypeInteger, 1, 1, 10)
    }),
    [this](const PropertyList &properties) -> ReturnValue {
        int sides = properties["sides"].value<int>();
        int count = properties["count"].value<int>();
        int total = 0;
        std::string rolls;
        for (int i = 0; i < count; i++) {
            int r = (esp_random() % sides) + 1;
            total += r;
            rolls += (i ? "," : "") + std::to_string(r);
        }
        return std::string("{\"rolls\":[") + rolls + "],\"total\":" + std::to_string(total) + "}";
    });
	
//таймер
	        mcp_server.AddTool("self.timer.set",
            "Set a countdown timer. Call when the user asks to set a timer or remind after some time. "
            "seconds is the total time in seconds (convert minutes and hours to seconds). "
            "A new timer replaces the previous one. Confirm the time to the user.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 60, 1, 86400)}),
            [this](const PropertyList &properties) -> ReturnValue {
                int seconds = properties["seconds"].value<int>();
                EnsureTimer();
                esp_timer_stop(timer_handle_);
                esp_timer_start_once(timer_handle_, (int64_t)seconds * 1000000);
                timer_end_us_ = esp_timer_get_time() + (int64_t)seconds * 1000000;
                return std::string("{\"success\":true,\"seconds\":") + std::to_string(seconds) + "}";
            });

        mcp_server.AddTool("self.timer.cancel",
            "Cancel the running timer. Call when the user asks to cancel or stop the timer.",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                if (timer_handle_) esp_timer_stop(timer_handle_);
                timer_end_us_ = 0;
                return std::string("{\"success\":true}");
            });

        mcp_server.AddTool("self.timer.left",
            "Get how many seconds are left on the timer. Call when the user asks how much time is left.",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                int64_t left_us = timer_end_us_ - esp_timer_get_time();
                if (timer_end_us_ == 0 || left_us <= 0) {
                    return std::string("{\"running\":false}");
                }
                return std::string("{\"running\":true,\"seconds_left\":") + std::to_string((int)(left_us / 1000000)) + "}";
            });

        mcp_server.AddTool("self.stopwatch.start",
            "Start the stopwatch. Call when the user asks to start the stopwatch or start counting time.",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                stopwatch_start_us_ = esp_timer_get_time();
                stopwatch_running_ = true;
                return std::string("{\"success\":true}");
            });

        mcp_server.AddTool("self.stopwatch.stop",
            "Stop the stopwatch and get the elapsed time in seconds. "
            "Call when the user asks to stop the stopwatch or asks how much time has passed. "
            "Announce the time in minutes and seconds.",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                if (!stopwatch_running_) {
                    return std::string("{\"running\":false}");
                }
                stopwatch_running_ = false;
                int elapsed = (int)((esp_timer_get_time() - stopwatch_start_us_) / 1000000);
                return std::string("{\"elapsed_seconds\":") + std::to_string(elapsed) + "}";
            });
			
// выключение по голосу
mcp_server.AddTool(
    "self.power.shutdown",
    "Turn off the device completely (shut down, power off). "
    "Call this ONLY when the user gives an explicit command to turn off, "
    "shut down, or power off the device — for example \"выключись\", "
    "\"выключи себя\", \"отключись\", \"turn off\", \"shut down\". "
    "end the conversation, they don't mean shut down. "
    "Before calling this, say goodbye to the user out loud in their language, "
    "because the device will power off a couple of seconds after this call "
    "and won't be able to speak again until turned back on.",
    PropertyList(),
      [this](const PropertyList &) -> ReturnValue {
        display_->SetShuttingDown(true);
        display_->DrawFace(FACE_SAD);
        vTaskDelay(pdMS_TO_TICKS(1500));
        display_->DrawFace(FACE_DEAD);
        vTaskDelay(pdMS_TO_TICKS(1500));
        power_->PowerAudioOff();
        power_->PowerEpdOff();
        power_->VbatPowerOff();
        return std::string("{\"success\":true}");
    });
 
// остановить прослушивание по голосу
mcp_server.AddTool(
    "self.chat.stop_listening",
    "Stop listening / pause the conversation. "
    "Call this when the user explicitly asks to stop listening, pause, "
    "be quiet, or stop the conversation — for example \"хватит слушать\", "
    "\"перестань слушать\", \"остановись\", \"stop listening\", \"pause\". "
    "Do NOT call this for a casual goodbye like \"пока\" or \"bye\" unless "
    "the user clearly wants listening to stop, not just to end the current reply. "
    "This does not turn off the device, only stops it from listening until "
    "the user presses the button or gives a new command.",
    PropertyList(),
    [this](const PropertyList &) -> ReturnValue {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            app.ToggleChatState();
        }
        return std::string("{\"success\":true}");
    });

// режим часов по голосу
        mcp_server.AddTool(
            "self.power.desk_mode",
            "Switch the device into the autonomous desk clock mode (deep sleep). "
            "The screen then shows a clock, the room climate and the weather and "
            "updates itself every minute, while the voice assistant is off until "
            "the user presses the PWR button. "
            "Call this when the user asks to go to sleep mode, desk mode or clock "
            "mode, for example \"режим часов\", \"спящий режим\", \"засыпай\", "
            "\"усни\", \"режим экономии\", \"go to sleep\". "
            "Do NOT call this for turning the device off, use self.power.shutdown for that. "
            "After calling, say a very short goodbye in the user's language, the "
            "device switches right after it finishes speaking.",
            PropertyList(),
            [this](const PropertyList &) -> ReturnValue {
                RequestDeskMode(true);
                return std::string("{\"success\":true}");
            });
    }

    void InitializeLcdDisplay(bool quick_start = false) {
        custom_lcd_spi_t lcd_spi_data = {};
        lcd_spi_data.cs               = EPD_CS_PIN;
        lcd_spi_data.dc               = EPD_DC_PIN;
        lcd_spi_data.rst              = EPD_RST_PIN;
        lcd_spi_data.busy             = EPD_BUSY_PIN;
        lcd_spi_data.mosi             = EPD_MOSI_PIN;
        lcd_spi_data.scl              = EPD_SCK_PIN;
        lcd_spi_data.spi_host         = EPD_SPI_NUM;
        lcd_spi_data.buffer_len       = 5000;
        display_                      = new CustomLcdDisplay(NULL, NULL, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, lcd_spi_data, quick_start);
    }

    void Power_Init() {
        power_ = new BoardPowerBsp(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
        power_->VbatPowerOn();
        power_->PowerAudioOn();
        power_->PowerEpdOn();
        // После режима часов линии питания и светодиод могли остаться
        // "замороженными" на время сна. Уровни уже выставлены выше,
        // теперь отпускаем заморозку.
        gpio_hold_dis(VBAT_PWR_PIN);
        gpio_hold_dis(EPD_PWR_PIN);
        gpio_hold_dis(Audio_PWR_PIN);
        gpio_hold_dis(GPIO_NUM_3);
        gpio_deep_sleep_hold_dis();
        do {
            vTaskDelay(pdMS_TO_TICKS(10));
        } while (!gpio_get_level(VBAT_PWR_GPIO));
    }
	
	       void EnsureTimer() {
        if (timer_handle_) return;
        esp_timer_create_args_t args = {};
        args.callback = [](void *arg) {
            auto *self = static_cast<CustomBoard *>(arg);
            Application::GetInstance().Schedule([self]() { self->StartAlarm(); });
        };
        args.arg = this;
        args.name = "user_timer";
        esp_timer_create(&args, &timer_handle_);
    }

// Проверяет, готово ли устройство, и если да, запускает прослушку.
    // Вызывается таймером раз в секунду.
    void TryAutoListen() {
        auto& app = Application::GetInstance();
        autolisten_retries_++;
 
        // kDeviceStateIdle означает: загрузка закончена, Wi-Fi поднят,
        // протокол готов, устройство просто ждёт команды.
               if (app.GetDeviceState() == kDeviceStateIdle) {
            ESP_LOGI(TAG, "Auto listen: device ready, saying hello");
            esp_timer_stop(autolisten_timer_);
            StartSntpOnce();
            app.WakeWordInvoke("Привет!!!"); //Говорит после пробуждения
            return;
        }
 
        // Если за 30 секунд так и не дождались готовности (нет Wi-Fi,
        // режим настройки и т.п.), прекращаем попытки, чтобы таймер
        // не крутился вечно.
        if (autolisten_retries_ >= 30) {
            ESP_LOGW(TAG, "Auto listen: device not ready after 30s, giving up");
            esp_timer_stop(autolisten_timer_);
        }
    }
 //экран загрузки, неблокирующий: кадры идут по таймеру,
 //параллельно с подключением к сети
 static constexpr int kBootFrameMs = 1800;   // обычное включение: 6 кадров по 1.8 с - ВРЕМЯ ЭКРАНА ЗАГРУЗКИ
 static constexpr int kBootTickMs = 300;     // шаг таймера анимации загрузки

 // Кадр анимации загрузки: целое лицо (full >= 0) или глаза + рот.
 struct BootStep {
     int16_t full;
     int8_t eyes;
     int8_t mouth;
     int16_t ms;
 };

 // Обычное включение: прежняя анимация с рамкой.
 static constexpr BootStep kColdBoot[] = {
     { FACE_BOOT_0, -1, -1, kBootFrameMs }, { FACE_BOOT_1, -1, -1, kBootFrameMs },
     { FACE_BOOT_2, -1, -1, kBootFrameMs }, { FACE_BOOT_3, -1, -1, kBootFrameMs },
     { FACE_BOOT_4, -1, -1, kBootFrameMs }, { FACE_BOOT_5, -1, -1, kBootFrameMs },
 };

 // Выход из режима часов: просыпается. Спит, приоткрывает глаз, зевает,
 // моргает, оглядывается и улыбается. Идёт, пока подключается Wi-Fi.
 static constexpr BootStep kWakeUp[] = {
     { -1, EYES_SLEEP_Z1, MOUTH_FLAT,  1200 },
     { -1, EYES_SLEEP_Z2, MOUTH_FLAT,  1200 },
     { -1, EYES_WINK_L,   MOUTH_FLAT,  1200 },   // приоткрыл один глаз
     { -1, EYES_CLOSED,   MOUTH_YAWN,  1800 },   // зевает
     { -1, EYES_SLEEPY,   MOUTH_FLAT,  1200 },
     { -1, EYES_CLOSED,   MOUTH_IDLE,   600 },   // моргнул
     { -1, EYES_OPEN,     MOUTH_IDLE,   600 },
     { -1, EYES_LOOK_L,   MOUTH_IDLE,  1200 },   // огляделся
     { -1, EYES_LOOK_R,   MOUTH_IDLE,  1200 },
     { -1, EYES_HAPPY,    MOUTH_SMILE, 1200 },   // привет!
 };

 const BootStep* boot_steps_ = kColdBoot;
 int boot_step_count_ = 0;
 int boot_step_left_ms_ = 0;
 bool woke_from_desk_ = false;   // этот запуск: выход из режима часов

 void DrawBootStep(const BootStep& st) {
     if (st.full >= 0) {
         display_->DrawFace(st.full);
     } else {
         display_->DrawFaceParts(st.eyes, st.mouth);
     }
 }

 void ShowBootProgress() {
    display_->SetBooting(true);
    if (woke_from_desk_) {
        boot_steps_ = kWakeUp;
        boot_step_count_ = sizeof(kWakeUp) / sizeof(kWakeUp[0]);
    } else {
        boot_steps_ = kColdBoot;
        boot_step_count_ = sizeof(kColdBoot) / sizeof(kColdBoot[0]);
    }
    boot_frame_ = 0;
    boot_step_left_ms_ = boot_steps_[0].ms;
    DrawBootStep(boot_steps_[0]);

    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        static_cast<CustomBoard*>(arg)->NextBootFrame();
    };
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "boot_anim";
    args.skip_unhandled_events = true;

    if (esp_timer_create(&args, &boot_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Boot anim: failed to create timer");
        display_->SetBooting(false);
        return;
    }
    esp_timer_start_periodic(boot_timer_, kBootTickMs * 1000);
}

 void NextBootFrame() {
    boot_step_left_ms_ -= kBootTickMs;
    if (boot_step_left_ms_ > 0) return;   // текущий кадр ещё показываем

    boot_frame_++;
    if (boot_frame_ >= boot_step_count_) {
        esp_timer_stop(boot_timer_);
        display_->SetBooting(false);
        // Если за время загрузки он уже успел заговорить или слушает,
        // сразу рисуем правильное лицо, а не обычное.
        auto state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateListening) {
            display_->SetStatus(Lang::Strings::LISTENING);
        } else if (state == kDeviceStateSpeaking) {
            display_->SetStatus(Lang::Strings::SPEAKING);
        } else {
            display_->RequestFace();
        }
        return;
    }
    boot_step_left_ms_ = boot_steps_[boot_frame_].ms;
    DrawBootStep(boot_steps_[boot_frame_]);
}
 
    // Запускает периодическую проверку готовности устройства.
    void StartAutoListen() {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<CustomBoard*>(arg)->TryAutoListen();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "autolisten";
        args.skip_unhandled_events = true;
 
        if (esp_timer_create(&args, &autolisten_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Auto listen: failed to create timer");
            return;
        }
        // Раз в секунду, первая проверка через секунду после старта.
        esp_timer_start_periodic(autolisten_timer_, 1000 * 1000);
    }

    void StartAlarm() {
        if (!alarm_handle_) {
            esp_timer_create_args_t args = {};
            args.callback = [](void *arg) {
                auto *self = static_cast<CustomBoard *>(arg);
                Application::GetInstance().Schedule([self]() { self->AlarmTick(); });
            };
            args.arg = this;
            args.name = "alarm_repeat";
            esp_timer_create(&args, &alarm_handle_);
        }
        esp_timer_stop(alarm_handle_);
        alarm_count_ = 0;
        auto codec = GetAudioCodec();
        saved_volume_ = codec->output_volume();
        codec->SetOutputVolume(100);
        esp_timer_start_periodic(alarm_handle_, 1000000);
    }

    void AlarmTick() {
        if (alarm_count_ >= 5) {
            esp_timer_stop(alarm_handle_);
            GetAudioCodec()->SetOutputVolume(saved_volume_);
            return;
        }
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        alarm_count_++;
    }

    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);
    
            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config);
    
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }

        if (initialized) {
            int raw_value = 0;
            int raw_voltage = 0;
            int voltage = 0; // mV
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage);
            voltage =  raw_voltage * 2;
            // ESP_LOGI(TAG, "voltage: %dmV", voltage);
            return (uint16_t)voltage;
        }

        return 0;
    }

    uint8_t BatterygetPercent() {
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }

        voltage /= 10;
        int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        // ESP_LOGI(TAG, "voltage: %dmV, percentage: %d%%", voltage, percent);
        return (uint8_t)percent;
    }

    bool ReadSHTC3(float &temperature, float &humidity) {
        static i2c_master_dev_handle_t shtc3_dev = nullptr;

        if (shtc3_dev == nullptr) {
            i2c_device_config_t dev_cfg = {};
            dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            dev_cfg.device_address  = 0x70;
            dev_cfg.scl_speed_hz    = 100000;
            if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &shtc3_dev) != ESP_OK) {
                ESP_LOGE(TAG, "SHTC3: failed to add device");
                return false;
            }
        }

        // Пробуждение. Раньше тут стояло pdMS_TO_TICKS(2), что при тике
        // в десять миллисекунд давало ноль тиков, и датчик не успевал
        // проснуться до команды измерения.
        uint8_t wakeup[2] = {0x35, 0x17};
        i2c_master_transmit(shtc3_dev, wakeup, 2, 100);
        vTaskDelay(pdMS_TO_TICKS(20));

        // Измерение, обычный режим, без растягивания такта.
        uint8_t cmd[2] = {0x78, 0x66};
        esp_err_t cerr = ESP_FAIL;
        for (int attempt = 0; attempt < 3; attempt++) {
            cerr = i2c_master_transmit(shtc3_dev, cmd, 2, 100);
            if (cerr == ESP_OK) {
                break;
            }
            i2c_master_transmit(shtc3_dev, wakeup, 2, 100);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (cerr != ESP_OK) {
            ESP_LOGE(TAG, "SHTC3: measure cmd failed");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        uint8_t data[6] = {0};
        esp_err_t rerr = ESP_FAIL;
        for (int attempt = 0; attempt < 3; attempt++) {
            rerr = i2c_master_receive(shtc3_dev, data, 6, 100);
            if (rerr == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "SHTC3: read failed");
            return false;
        }

        // Усыпляем датчик до следующего чтения.
        uint8_t sleep_cmd[2] = {0xB0, 0x98};
        i2c_master_transmit(shtc3_dev, sleep_cmd, 2, 100);

        uint16_t raw_t = (data[0] << 8) | data[1];
        uint16_t raw_h = (data[3] << 8) | data[4];

        temperature = -45.0f + 175.0f * (float)raw_t / 65535.0f - 4.7f;
        humidity    = 100.0f * (float)raw_h / 65535.0f + 3.0f;

        ESP_LOGI(TAG, "SHTC3: %.1f C, %.1f %%", temperature, humidity);
        return true;
    }
	

    // ================================================================
    // ===================== РЕЖИМ ЧАСОВ (deep sleep) ==================
    // ================================================================
    //
    // Как это работает:
    // 1. Короткое нажатие PWR или голосом "режим часов": рисуем часы,
    //    выключаем звук и экран и засыпаем глубоким сном.
    // 2. Раз в минуту просыпаемся, обновляем экран без мигания и снова спим.
    //    Ассистент при этом не запускается, поэтому это быстро.
    // 3. Раз в сутки при пробуждении подключаемся к Wi-Fi, берём точное
    //    время и погоду. Нет Wi-Fi: показываем что есть, попробуем через сутки.
    // 4. BOOT во сне: следующая страница (часы, погода, комната).
    // 5. PWR коротко во сне: выход в обычный режим с ассистентом.
    //    PWR долго во сне: выключение.

    // Запоминаем нарисованный кадр, чтобы после сна обновлять только разницу.
    void DeskSaveFrame(bool was_full) {
        int n = display_->FrameSize();
        if (n > kDeskFrameBytes) n = kDeskFrameBytes;
        memcpy(s_desk.frame, display_->FrameBuffer(), n);
        s_desk.frame_valid = true;
        s_desk.updates_since_full = was_full ? 0 : s_desk.updates_since_full + 1;
    }

    // Ждёт, пока отпустят кнопку. Возвращает, сколько её держали (мс).
    int WaitRelease(gpio_num_t pin, int max_ms) {
        int held = 0;
        while (gpio_get_level(pin) == 0 && held < max_ms) {
            vTaskDelay(pdMS_TO_TICKS(20));
            held += 20;
        }
        return held;
    }

    // Сколько спать до следующего обновления: просыпаемся сразу после
    // смены минуты, чтобы часы на экране не отставали.
    int64_t DeskMicrosToNextWake() {
        const int64_t period = (int64_t)kDeskWakeSec * 1000000;
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (tv.tv_sec < 1600000000) {
            return period;   // времени нет, просто через период
        }
        int64_t into = (int64_t)(tv.tv_sec % kDeskWakeSec) * 1000000 + tv.tv_usec;
        int64_t us = period - into + 300000;
        if (us < 300000) us = 300000;
        return us;
    }

    // Засыпание. with_timer = false: спим до нажатия PWR, без будильника
    // (так выглядит "выключено", когда питание идёт от USB).
    // keep_power = false: отпустить защёлку питания от батареи.
    void DeskGoToSleep(bool with_timer, bool keep_power) {
        // Кнопки должны быть отпущены, иначе сразу же проснёмся.
        WaitRelease(BOOT_BUTTON_GPIO, 3000);
        WaitRelease(VBAT_PWR_GPIO, 3000);

        gpio_set_level(Audio_PWR_PIN, 1);                 // звук и датчик выключены
        gpio_set_level(EPD_PWR_PIN, 1);                   // экран выключен, картинка остаётся
        gpio_set_level(VBAT_PWR_PIN, keep_power ? 1 : 0); // защёлка питания от батареи

        // Замораживаем эти линии на время сна, иначе они "поплывут"
        // и плата от батареи просто выключится.
        gpio_hold_en(VBAT_PWR_PIN);
        gpio_hold_en(EPD_PWR_PIN);
        gpio_hold_en(Audio_PWR_PIN);
        gpio_hold_en(GPIO_NUM_3);                         // светодиод остаётся погашенным
        gpio_deep_sleep_hold_en();

        // Будим кнопками. Кнопку, которая почему-то зажата, не берём,
        // иначе устройство будет просыпаться без конца.
        uint64_t mask = 0;
        if (gpio_get_level(VBAT_PWR_GPIO) == 1) {
            mask |= 1ULL << VBAT_PWR_GPIO;
        }
        if (with_timer && gpio_get_level(BOOT_BUTTON_GPIO) == 1) {
            mask |= 1ULL << BOOT_BUTTON_GPIO;
        }
        if (mask != 0) {
            esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
            if (mask & (1ULL << VBAT_PWR_GPIO)) {
                rtc_gpio_pullup_en(VBAT_PWR_GPIO);
                rtc_gpio_pulldown_dis(VBAT_PWR_GPIO);
            }
            if (mask & (1ULL << BOOT_BUTTON_GPIO)) {
                rtc_gpio_pullup_en(BOOT_BUTTON_GPIO);
                rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO);
            }
        }

        if (with_timer) {
            int64_t us = DeskMicrosToNextWake();
            esp_sleep_enable_timer_wakeup(us);
            ESP_LOGI(TAG, "Desk: sleep for %lld ms", (long long)(us / 1000));
        } else {
            ESP_LOGI(TAG, "Desk: sleep until PWR");
        }
        esp_deep_sleep_start();
    }

    // Включение питания при пробуждении. Уровни выставляем ДО того, как
    // отпустить заморозку, чтобы защёлка батареи не мигнула в ноль.
    void DeskPowerOn() {
        gpio_config_t io = {};
        io.mode = GPIO_MODE_OUTPUT;
        io.pin_bit_mask = (1ULL << VBAT_PWR_PIN) | (1ULL << EPD_PWR_PIN) | (1ULL << Audio_PWR_PIN);
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io);
        gpio_set_level(VBAT_PWR_PIN, 1);    // питание от батареи держим
        gpio_set_level(EPD_PWR_PIN, 0);     // экран включён
        gpio_set_level(Audio_PWR_PIN, 0);   // эту линию тоже, на ней может сидеть датчик
        gpio_hold_dis(VBAT_PWR_PIN);
        gpio_hold_dis(EPD_PWR_PIN);
        gpio_hold_dis(Audio_PWR_PIN);
        gpio_deep_sleep_hold_dis();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Собирает данные для экрана: датчик, батарея, погода из памяти.
    void DeskCollect(DeskScreenData& d) {
        d.weather = RtcToWeather(s_desk.weather);
        float t = 0, h = 0;
        d.room_ok = ReadSHTC3(t, h);
        d.room_t = t;
        d.room_h = h;
        d.battery = (int)BatterygetPercent();
        HistRecord(d.room_ok, d.room_t, d.room_h, d.battery);
        FillChartData(d);
    }

    // ===== Виджеты во время разговора =====
    // Любая страница режима часов по голосу. Сначала собираем свежие данные,
    // потом отдаём экрану (он нарисует сам) и возвращаем цифры для ответа.

    static int PageFromName(const std::string& name) {
        struct { const char* name; int page; } kNames[] = {
            { "clock", DESK_PAGE_CLOCK },       { "time", DESK_PAGE_CLOCK },
            { "weather", DESK_PAGE_WEATHER },   { "forecast", DESK_PAGE_OUT24 },
            { "rain", DESK_PAGE_OUT24 },        { "room", DESK_PAGE_ROOM },
            { "room_history", DESK_PAGE_ROOM24 }, { "ventilate", DESK_PAGE_BOTH },
            { "sun", DESK_PAGE_SUN },           { "moon", DESK_PAGE_MOON },
            { "battery", DESK_PAGE_BATTERY },
        };
        for (auto& n : kNames) {
            if (name == n.name) return n.page;
        }
        return -1;
    }

    void CollectLiveData(DeskScreenData& d, int page) {
        bool need_weather = (page == DESK_PAGE_CLOCK || page == DESK_PAGE_WEATHER ||
                             page == DESK_PAGE_OUT24 || page == DESK_PAGE_BOTH ||
                             page == DESK_PAGE_SUN);
        if (need_weather) {
            WeatherCache w;
            if (GetWeather(w)) d.weather = w;   // из кэша или свежая из сети
        } else if (s_weather_cache.valid) {
            d.weather = s_weather_cache;
        }
        float t = 0, h = 0;
        d.room_ok = ReadSHTC3(t, h);
        d.room_t = t;
        d.room_h = h;
        d.battery = (int)BatterygetPercent();
        HistRecord(d.room_ok, d.room_t, d.room_h, d.battery);
        FillChartData(d);
        if (d.now == 0) d.now = (int64_t)time(nullptr);
    }

    static void MinMax(const float* v, int n, float& lo, float& hi, int& count) {
        lo = 1e9f; hi = -1e9f; count = 0;
        for (int i = 0; i < n; i++) {
            if (std::isnan(v[i])) continue;
            if (v[i] < lo) lo = v[i];
            if (v[i] > hi) hi = v[i];
            count++;
        }
    }

    static std::string HourMin(int64_t t) {
        time_t tt = (time_t)t;
        struct tm tm_l;
        localtime_r(&tt, &tm_l);
        char buf[8];
        strftime(buf, sizeof(buf), "%H:%M", &tm_l);
        return buf;
    }

    // Короткие данные о показанной странице, чтобы ассистенту было что сказать.
    static std::string DescribePage(int page, const DeskScreenData& d) {
        char buf[256];
        bool time_ok = IsTimeValidNow();
        switch (page) {
            case DESK_PAGE_CLOCK: {
                if (!time_ok) return "{\"success\":false,\"error\":\"time is not known yet\"}";
                time_t tt = (time_t)d.now;
                struct tm tm_l;
                localtime_r(&tt, &tm_l);
                char tb[48];
                strftime(tb, sizeof(tb), "\"time\":\"%H:%M\",\"date\":\"%Y-%m-%d\",\"weekday\":\"%A\"", &tm_l);
                return std::string("{\"success\":true,") + tb + "}";
            }
            case DESK_PAGE_WEATHER:
                if (!d.weather.valid) return "{\"success\":false,\"error\":\"no weather data\"}";
                return d.weather.summary;
            case DESK_PAGE_OUT24: {
                float lo, hi; int n;
                MinMax(d.out_next, 25, lo, hi, n);
                if (n == 0) return "{\"success\":false,\"error\":\"no forecast yet\"}";
                int rain_max = -1, rain_at = 0;
                for (int i = 0; i < 24; i++) {
                    if (!std::isnan(d.rain_next[i]) && (int)d.rain_next[i] > rain_max) {
                        rain_max = (int)d.rain_next[i];
                        rain_at = i;
                    }
                }
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"next24h_min_c\":%.0f,\"next24h_max_c\":%.0f,"
                    "\"max_rain_chance_percent\":%d,\"max_rain_in_hours\":%d}",
                    lo, hi, rain_max < 0 ? 0 : rain_max, rain_at);
                return buf;
            }
            case DESK_PAGE_ROOM:
                if (!d.room_ok) return "{\"success\":false,\"error\":\"sensor did not answer\"}";
                snprintf(buf, sizeof(buf), "{\"success\":true,\"temperature\":%.1f,\"humidity\":%.0f}",
                         d.room_t, d.room_h);
                return buf;
            case DESK_PAGE_ROOM24: {
                float tlo, thi, hlo, hhi; int nt, nh;
                MinMax(d.room_t_hist, DESK_ROOM_POINTS, tlo, thi, nt);
                MinMax(d.room_h_hist, DESK_ROOM_POINTS, hlo, hhi, nh);
                if (nt == 0) return "{\"success\":true,\"note\":\"history is still being collected, the chart fills up during the day\"}";
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"temp_min_24h\":%.1f,\"temp_max_24h\":%.1f,"
                    "\"humidity_min_24h\":%.0f,\"humidity_max_24h\":%.0f,\"hours_of_data\":%d}",
                    tlo, thi, nh ? hlo : 0.0f, nh ? hhi : 0.0f, nt / 6);
                return buf;
            }
            case DESK_PAGE_BOTH: {
                if (!d.room_ok || !d.weather.valid) {
                    return "{\"success\":false,\"error\":\"need both room sensor and weather\"}";
                }
                bool cooler = d.weather.temperature < d.room_t;
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"home_c\":%.1f,\"outside_c\":%.1f,\"outside_is_cooler\":%s,"
                    "\"note\":\"hatched areas on the chart show when it was cooler outside, a good time to open windows\"}",
                    d.room_t, d.weather.temperature, cooler ? "true" : "false");
                return buf;
            }
            case DESK_PAGE_SUN: {
                if (!d.sun_ok) return "{\"success\":false,\"error\":\"no sun data yet\"}";
                std::string rise = HourMin(d.sunrise), set = HourMin(d.sunset);
                int len_min = (int)((d.sunset - d.sunrise) / 60);
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"sunrise\":\"%s\",\"sunset\":\"%s\",\"day_length\":\"%dh%02dm\",\"uv_index_max\":%.1f}",
                    rise.c_str(), set.c_str(), len_min / 60, len_min % 60, d.uv_max < 0 ? 0.0f : d.uv_max);
                return buf;
            }
            case DESK_PAGE_MOON: {
                if (!time_ok) return "{\"success\":false,\"error\":\"time is not known yet\"}";
                const double kSynodic = 29.530588853;
                const double kNewMoonRef = 947182440.0;
                double age = fmod(((double)d.now - kNewMoonRef) / 86400.0, kSynodic);
                if (age < 0) age += kSynodic;
                double phase = age / kSynodic;
                int illum = (int)lround((1.0 - cos(2.0 * M_PI * phase)) / 2.0 * 100.0);
                const char* name;
                if (phase < 0.03 || phase > 0.97) name = "new moon";
                else if (phase < 0.22) name = "waxing crescent";
                else if (phase < 0.28) name = "first quarter";
                else if (phase < 0.47) name = "waxing gibbous";
                else if (phase < 0.53) name = "full moon";
                else if (phase < 0.72) name = "waning gibbous";
                else if (phase < 0.78) name = "last quarter";
                else name = "waning crescent";
                int to_full = (int)lround(fmod(0.5 - phase + 1.0, 1.0) * kSynodic);
                int to_new = (int)lround(fmod(1.0 - phase, 1.0) * kSynodic);
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"phase\":\"%s\",\"illumination_percent\":%d,"
                    "\"days_to_full_moon\":%d,\"days_to_new_moon\":%d}",
                    name, illum, to_full, to_new);
                return buf;
            }
            case DESK_PAGE_BATTERY:
                if (d.batt_days_left >= 0) {
                    snprintf(buf, sizeof(buf), "{\"percent\":%d,\"days_left_estimate\":%d}",
                             d.battery, d.batt_days_left);
                } else {
                    snprintf(buf, sizeof(buf), "{\"percent\":%d,\"days_left_estimate\":\"not enough history yet\"}",
                             d.battery);
                }
                return buf;
        }
        return "{\"success\":false}";
    }

    // Подключение к Wi-Fi во сне, без запуска ассистента.
    // Берём сети, которые уже сохранены при настройке устройства.
    bool DeskWifiConnect() {
        const auto& list = SsidManager::GetInstance().GetSsidList();
        if (list.empty()) {
            ESP_LOGW(TAG, "Desk: no saved Wi-Fi");
            return false;
        }

        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = false;
        if (esp_wifi_init(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "Desk: wifi init failed");
            return false;
        }
        s_desk_wifi_events = xEventGroupCreate();
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &DeskWifiEvent, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &DeskWifiEvent, nullptr);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();

        int networks = (int)list.size();
        if (networks > 2) networks = 2;   // больше двух сетей не перебираем, бережём батарею
        for (int i = 0; i < networks; i++) {
            wifi_config_t wc = {};
            size_t sl = list[i].ssid.size();
            if (sl > sizeof(wc.sta.ssid)) sl = sizeof(wc.sta.ssid);
            memcpy(wc.sta.ssid, list[i].ssid.data(), sl);
            size_t pl = list[i].password.size();
            if (pl >= sizeof(wc.sta.password)) pl = sizeof(wc.sta.password) - 1;
            memcpy(wc.sta.password, list[i].password.data(), pl);
            esp_wifi_set_config(WIFI_IF_STA, &wc);

            ESP_LOGI(TAG, "Desk: connecting to %s", list[i].ssid.c_str());
            int64_t deadline = esp_timer_get_time() + (int64_t)kDeskWifiTimeoutMs * 1000;
            xEventGroupClearBits(s_desk_wifi_events, kDeskWifiGotIp | kDeskWifiLost);
            esp_wifi_connect();
            while (true) {
                int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
                if (left_ms <= 0) break;
                EventBits_t bits = xEventGroupWaitBits(s_desk_wifi_events,
                    kDeskWifiGotIp | kDeskWifiLost, pdTRUE, pdFALSE, pdMS_TO_TICKS(left_ms));
                if (bits & kDeskWifiGotIp) {
                    ESP_LOGI(TAG, "Desk: Wi-Fi connected");
                    return true;
                }
                if (bits & kDeskWifiLost) {
                    // не получилось с первого раза, пробуем ещё, пока есть время
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_wifi_connect();
                }
            }
            esp_wifi_disconnect();
        }
        ESP_LOGW(TAG, "Desk: Wi-Fi not available");
        return false;
    }

    // Раз в сутки: точное время и свежая погода.
    void DeskSync() {
        // Попытка одна в сутки, даже неудачная: без бесконечных повторов,
        // чтобы не посадить батарею, если роутер выключен.
        s_desk.last_sync = time(nullptr);

        if (DeskWifiConnect()) {
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sntp_cfg);
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000)) == ESP_OK) {
                s_sntp_synced = true;
                ESP_LOGI(TAG, "Desk: time synced");
            } else {
                ESP_LOGW(TAG, "Desk: time sync failed");
            }
            esp_netif_sntp_deinit();

            if (FetchWeather(WifiBoard::GetNetwork())) {
                s_desk.weather = WeatherToRtc(s_weather_cache);
                ESP_LOGI(TAG, "Desk: weather updated");
            }
            // время могло сильно поменяться после синхронизации
            s_desk.last_sync = time(nullptr);
        }
        esp_wifi_stop();
    }

    // Долгое нажатие PWR во сне: выключение.
    void DeskShutdown() {
        InitializeLcdDisplay(true);
        display_->RenderFaceAfterWake(FACE_DEAD, s_desk.frame_valid ? s_desk.frame : nullptr);
        display_->EPD_Sleep();
        s_desk.active = false;
        s_desk.frame_valid = false;

        gpio_set_level(EPD_PWR_PIN, 1);
        gpio_set_level(Audio_PWR_PIN, 1);
        gpio_set_level(VBAT_PWR_PIN, 0);   // отпускаем питание от батареи
        vTaskDelay(pdMS_TO_TICKS(2000));

        // Если мы всё ещё живы, значит питание от USB. Тогда спим
        // до нажатия PWR, после него будет обычный запуск.
        DeskGoToSleep(false, false);
    }

    // Главное: что делать при пробуждении в режиме часов.
    // Возвращается, только если пора выйти в обычный режим.
    void RunDeskWake() {
        uint64_t pins = esp_sleep_get_ext1_wakeup_status();
        bool by_pwr  = (pins & (1ULL << VBAT_PWR_GPIO)) != 0;
        bool by_boot = (pins & (1ULL << BOOT_BUTTON_GPIO)) != 0;
        ESP_LOGI(TAG, "Desk wake: pwr=%d boot=%d page=%d", by_pwr, by_boot, s_desk.page);

        DeskPowerOn();

        if (by_pwr) {
            int held = WaitRelease(VBAT_PWR_GPIO, 4000);
            if (held >= kDeskLongPressMs) {
                DeskShutdown();   // сюда не возвращаемся
            }
            ESP_LOGI(TAG, "Desk: exit to normal mode");
            s_desk.active = false;
            return;   // дальше обычный запуск с ассистентом
        }

        if (by_boot) {
            s_desk.page = (s_desk.page + 1) % DESK_PAGE_COUNT;
        }

        InitializeI2c();

        // Раз в сутки в сеть. По нажатию BOOT не ходим, чтобы страница
        // переключалась сразу, сходим при следующем обычном пробуждении.
        time_t now = time(nullptr);
        bool need_sync = !by_boot &&
            (s_desk.last_sync == 0 || now < s_desk.last_sync ||
             (now - s_desk.last_sync) >= kDeskSyncSec);
        if (need_sync) {
            DeskSync();
        }

        // Данные крупные (графики), поэтому в куче, а не на стеке.
        auto d = std::make_unique<DeskScreenData>();
        DeskCollect(*d);

        bool full = !s_desk.frame_valid || s_desk.updates_since_full >= kDeskFullRefreshEvery;
        InitializeLcdDisplay(true);
        // Полоска страниц видна, когда листаешь кнопкой BOOT. На следующем
        // обновлении через минуту она пропадает (или всегда, см. kDeskPageBarAlways).
        display_->RenderDeskPage(s_desk.page, *d, full ? nullptr : s_desk.frame,
                                 by_boot || kDeskPageBarAlways);
        DeskSaveFrame(full);
        display_->EPD_Sleep();

        DeskGoToSleep(true, true);
    }

    // Переход в режим часов из обычного режима.
    void RequestDeskMode(bool wait_for_speech) {
        if (desk_entering_) return;
        desk_entering_ = true;
        desk_wait_speech_ = wait_for_speech;
        xTaskCreate([](void* arg) {
            static_cast<CustomBoard*>(arg)->EnterDeskModeTask();
            vTaskDelete(nullptr);
        }, "desk_enter", 12288, this, 5, nullptr);
    }

    void EnterDeskModeTask() {
        auto& app = Application::GetInstance();

        // По голосу: даём договорить прощание. Ждём, пока начнёт говорить
        // (до 6 секунд), потом пока закончит (всего до 25 секунд).
        if (desk_wait_speech_) {
            int waited = 0;
            while (app.GetDeviceState() != kDeviceStateSpeaking && waited < 6000) {
                vTaskDelay(pdMS_TO_TICKS(200));
                waited += 200;
            }
            while (app.GetDeviceState() == kDeviceStateSpeaking && waited < 25000) {
                vTaskDelay(pdMS_TO_TICKS(200));
                waited += 200;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        ESP_LOGI(TAG, "Entering desk mode");

        // Останавливаем всё, что может рисовать лица.
        if (led_timer_) esp_timer_stop(led_timer_);
        if (boot_timer_) esp_timer_stop(boot_timer_);
        if (autolisten_timer_) esp_timer_stop(autolisten_timer_);
        display_->SetShuttingDown(true);
        // Засыпает, два кадра: глаза слипаются, потом спит с "Z".
        // Второй кадр остаётся на экране, пока собираются данные для часов.
        // Ждать чужое рисование не нужно: экран защищён общей блокировкой.
        display_->DrawFaceParts(EYES_SLEEPY, MOUTH_FLAT);
        vTaskDelay(pdMS_TO_TICKS(700));
        display_->DrawFaceParts(EYES_SLEEP_Z2, MOUTH_FLAT);

        // Данные для первого экрана. Сеть сейчас есть, погоду берём свежую.
        auto dp = std::make_unique<DeskScreenData>();
        DeskScreenData& d = *dp;
        WeatherCache w;
        if (GetWeather(w)) {
            d.weather = w;
        }
        float t = 0, h = 0;
        d.room_ok = ReadSHTC3(t, h);
        d.room_t = t;
        d.room_h = h;
        d.battery = (int)BatterygetPercent();
        HistRecord(d.room_ok, d.room_t, d.room_h, d.battery);
        FillChartData(d);

        memset(&s_desk, 0, sizeof(s_desk));
        s_desk.magic = kDeskMagic;
        s_desk.active = true;
        s_desk.page = DESK_PAGE_CLOCK;
        s_desk.weather = WeatherToRtc(d.weather);
        // Если точное время из интернета и погода уже есть, в сеть пойдём через сутки,
        // иначе при первом же пробуждении (заодно исправится неверное время).
        s_desk.last_sync = (s_sntp_synced && d.weather.valid) ? time(nullptr) : 0;

        power_->PowerAudioOff();
        power_->LedOff();

        display_->RenderDeskPage(DESK_PAGE_CLOCK, d, nullptr, true);   // полное обновление
        DeskSaveFrame(true);
        display_->EPD_Sleep();

        esp_wifi_stop();
        DeskGoToSleep(true, true);
    }

    // Точное время из интернета в обычном режиме. Запускается один раз,
    // дальше само подстраивается раз в час.
    void StartSntpOnce() {
        if (sntp_started_) return;
        sntp_started_ = true;
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        cfg.sync_cb = OnSntpSync;
        esp_netif_sntp_init(&cfg);
    }

  public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), pwr_button_(VBAT_PWR_GPIO) {
        setenv("TZ", kTimeZone, 1);
        tzset();
        HistEnsureLoaded();

        // Проснулись в режиме часов? Тогда обновляем экран и засыпаем обратно,
        // обычный запуск ассистента не нужен. Сюда возвращаемся, только если
        // нажали PWR, то есть пора выйти в обычный режим.
        if (s_desk.magic == kDeskMagic && s_desk.active) {
            if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
                RunDeskWake();
                woke_from_desk_ = true;   // сюда попадаем, только если вышли из режима часов
            } else {
                s_desk.active = false;   // перезагрузка не из сна: обычный режим
            }
        }

        // Погода, которую помнил режим часов, пригодится как запасная,
        // но при первом вопросе всё равно обновится из сети.
        if (s_desk.magic == kDeskMagic && s_desk.weather.valid && !s_weather_cache.valid) {
            s_weather_cache = RtcToWeather(s_desk.weather);
            s_weather_cache.fetched_at_ms = -kWeatherCacheTtlMs - 1;
        }

        Power_Init();
        InitializeI2c();
        InitializeButtons();
        InitializeTools();
        InitializeLcdDisplay();
        ShowBootProgress();
        StartAutoListen();
        StartListenLed();
    }

    virtual AudioCodec *GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        // Чувствительность микрофона. Ставится один раз, до первого включения
        // микрофона, поэтому применяется сразу.
        static bool gain_set = false;
        if (!gain_set) {
            gain_set = true;
            audio_codec.SetInputGain(kMicGainDb);
        }
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = false;
        discharging = !charging;
        level = (int)BatterygetPercent();

        return true;
    }
};

DECLARE_BOARD(CustomBoard);

