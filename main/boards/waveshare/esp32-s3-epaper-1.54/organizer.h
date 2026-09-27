// Органайзер: будильники, напоминания, списки (покупки и заметки).
// Хранится во флеше (NVS), поэтому переживает и сон, и выключение.
// Только логика, без экрана и звука: их делает плата.
#ifndef __ORGANIZER_H__
#define __ORGANIZER_H__

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <string>
#include <nvs.h>

#define ORG_MAX_ALARMS 8
#define ORG_MAX_REMINDERS 12
#define ORG_REM_TEXT 72        // байт на текст напоминания (UTF-8, ~35 русских букв)
#define ORG_LIST_MAX 24        // пунктов в списке
#define ORG_ITEM_LEN 44        // байт на пункт (~21 русская буква)

// Дни недели битами: 0 пн, 1 вт ... 6 вс. 0 = один раз.
#define ORG_DAYS_WEEKDAYS 0x1F
#define ORG_DAYS_WEEKEND  0x60
#define ORG_DAYS_ALL      0x7F

struct OrgAlarm {
    uint8_t used;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;        // дни недели, 0 = один раз (тогда время в once_at)
    int64_t once_at;
};

struct OrgReminder {
    uint8_t used;
    int64_t at;
    char text[ORG_REM_TEXT];
};

struct OrgSchedule {
    uint32_t magic;
    OrgAlarm alarms[ORG_MAX_ALARMS];
    OrgReminder rem[ORG_MAX_REMINDERS];
};

struct OrgList {
    uint32_t magic;
    uint8_t count;
    char items[ORG_LIST_MAX][ORG_ITEM_LEN];
};

enum OrgListId { ORG_LIST_SHOP = 0, ORG_LIST_NOTES = 1 };

enum OrgEventKind {
    ORG_EV_NONE = 0,
    ORG_EV_ALARM,
    ORG_EV_REMINDER,
    ORG_EV_TIMER,
    ORG_EV_SNOOZE,
};

struct OrgEvent {
    int kind = ORG_EV_NONE;
    int index = -1;       // номер будильника или напоминания
    int64_t at = 0;
};

static constexpr uint32_t kOrgMagic = 0x4F524731;   // "ORG1"
static constexpr uint32_t kOrgListMagic = 0x4C535431;   // "LST1"

// ===== флеш =====

static inline void OrgLoadSchedule(OrgSchedule& s) {
    memset(&s, 0, sizeof(s));
    s.magic = kOrgMagic;
    nvs_handle_t h;
    if (nvs_open("org", NVS_READONLY, &h) != ESP_OK) return;
    OrgSchedule tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, "sched", &tmp, &len) == ESP_OK && len == sizeof(tmp) && tmp.magic == kOrgMagic) {
        s = tmp;
    }
    nvs_close(h);
}

static inline bool OrgSaveSchedule(OrgSchedule& s) {
    s.magic = kOrgMagic;
    nvs_handle_t h;
    if (nvs_open("org", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "sched", &s, sizeof(s)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static inline const char* OrgListKey(int which) {
    return which == ORG_LIST_NOTES ? "notes" : "shop";
}

static inline void OrgLoadList(int which, OrgList& l) {
    memset(&l, 0, sizeof(l));
    l.magic = kOrgListMagic;
    nvs_handle_t h;
    if (nvs_open("org", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(l);
    OrgList tmp;
    if (nvs_get_blob(h, OrgListKey(which), &tmp, &len) == ESP_OK && len == sizeof(tmp) &&
        tmp.magic == kOrgListMagic && tmp.count <= ORG_LIST_MAX) {
        l = tmp;
    }
    nvs_close(h);
}

static inline bool OrgSaveList(int which, OrgList& l) {
    l.magic = kOrgListMagic;
    nvs_handle_t h;
    if (nvs_open("org", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, OrgListKey(which), &l, sizeof(l)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// ===== текст =====

// Копирует строку, обрезая по границе символа UTF-8 и убирая пробелы по краям.
static inline void OrgCopyText(char* dst, size_t cap, const char* src) {
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') src++;
    size_t n = strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' || src[n - 1] == '\n' || src[n - 1] == '\r' ||
                     src[n - 1] == '.')) n--;
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   // не рвём букву пополам
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

// Строчные буквы (латиница и кириллица), для поиска пунктов без учёта регистра.
static inline std::string OrgLower(const char* s) {
    std::string out;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p < 0x80) {
            char c = (char)*p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out += c;
            p++;
        } else if ((p[0] == 0xD0) && p[1]) {
            unsigned cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;        // А-Я -> а-я
            else if (cp == 0x401) cp = 0x451;                  // Ё -> ё
            if (cp == 0x451) cp = 0x435;                       // ё считаем как е
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
            p += 2;
        } else if (p[0] == 0xD1 && p[1]) {
            unsigned cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            if (cp == 0x451) cp = 0x435;
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
            p += 2;
        } else {
            out += (char)*p++;
        }
    }
    return out;
}

// ===== дни недели =====

static inline int OrgWeekdayBit(time_t t) {
    struct tm tm_l;
    localtime_r(&t, &tm_l);
    return (tm_l.tm_wday + 6) % 7;   // tm_wday: 0 = воскресенье
}

// Разбирает дни: once, daily, weekdays, weekends или список mon,tue,...
// Понимает и русские сокращения (пн, вт ...). -1 если не понял.
static inline int OrgParseDays(const char* s) {
    std::string v = OrgLower(s);
    if (v.empty() || v == "once" || v == "один раз" || v == "одинраз") return 0;
    if (v == "daily" || v == "everyday" || v == "every day" || v == "all") return ORG_DAYS_ALL;
    if (v == "weekdays" || v == "workdays" || v == "будни") return ORG_DAYS_WEEKDAYS;
    if (v == "weekends" || v == "weekend" || v == "выходные") return ORG_DAYS_WEEKEND;
    static const char* const kEn[7] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
    static const char* const kRu[7] = { "пн", "вт", "ср", "чт", "пт", "сб", "вс" };
    int mask = 0;
    size_t i = 0;
    while (i < v.size()) {
        while (i < v.size() && (v[i] == ',' || v[i] == ' ' || v[i] == ';')) i++;
        size_t j = i;
        while (j < v.size() && v[j] != ',' && v[j] != ' ' && v[j] != ';') j++;
        if (j > i) {
            std::string tok = v.substr(i, j - i);
            bool found = false;
            for (int d = 0; d < 7; d++) {
                if (tok.compare(0, 3, kEn[d]) == 0 || tok.compare(0, strlen(kRu[d]), kRu[d]) == 0) {
                    mask |= 1 << d;
                    found = true;
                }
            }
            if (!found) return -1;
        }
        i = j;
    }
    return mask == 0 ? -1 : mask;
}

// Дни словами для ответа ассистенту (по-английски, он переведёт).
static inline std::string OrgDaysText(int days) {
    if (days == 0) return "once";
    if (days == ORG_DAYS_ALL) return "every day";
    if (days == ORG_DAYS_WEEKDAYS) return "weekdays";
    if (days == ORG_DAYS_WEEKEND) return "weekends";
    static const char* const kEn[7] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
    std::string s;
    for (int d = 0; d < 7; d++) {
        if (days & (1 << d)) {
            if (!s.empty()) s += ",";
            s += kEn[d];
        }
    }
    return s;
}

// Дни коротко по-русски для экрана.
static inline std::string OrgDaysShortRu(int days) {
    if (days == ORG_DAYS_ALL) return "ежедн.";
    if (days == ORG_DAYS_WEEKDAYS) return "пн-пт";
    if (days == ORG_DAYS_WEEKEND) return "сб,вс";
    static const char* const kRu[7] = { "пн", "вт", "ср", "чт", "пт", "сб", "вс" };
    std::string s;
    for (int d = 0; d < 7; d++) {
        if (days & (1 << d)) {
            if (!s.empty()) s += ",";
            s += kRu[d];
        }
    }
    return s;
}

// ===== время событий =====

// Местное время hh:mm в день day_offset от дня времени base.
static inline int64_t OrgLocalAt(int64_t base, int day_offset, int hour, int minute) {
    time_t b = (time_t)base;
    struct tm tm_l;
    localtime_r(&b, &tm_l);
    tm_l.tm_mday += day_offset;
    tm_l.tm_hour = hour;
    tm_l.tm_min = minute;
    tm_l.tm_sec = 0;
    tm_l.tm_isdst = -1;   // летнее время определит сама
    return (int64_t)mktime(&tm_l);
}

// Ближайшее время hh:mm строго позже after (сегодня или завтра).
static inline int64_t OrgNextClock(int64_t after, int hour, int minute) {
    int64_t t = OrgLocalAt(after, 0, hour, minute);
    if (t <= after) t = OrgLocalAt(after, 1, hour, minute);
    return t;
}

// Следующий звонок будильника строго позже after. 0 = больше не зазвонит.
static inline int64_t OrgAlarmNext(const OrgAlarm& a, int64_t after) {
    if (!a.used) return 0;
    if (a.days == 0) return a.once_at > after ? a.once_at : 0;
    for (int k = 0; k <= 7; k++) {
        int64_t t = OrgLocalAt(after, k, a.hour, a.minute);
        if (t <= after) continue;
        if (a.days & (1 << OrgWeekdayBit((time_t)t))) return t;
    }
    return 0;
}

// Самое раннее событие в промежутке (from, to]. Таймер и отложенный
// будильник тоже события. kind = ORG_EV_NONE, если ничего нет.
static inline OrgEvent OrgFindDue(const OrgSchedule& s, int64_t from, int64_t to,
                                  int64_t timer_end, int64_t snooze_until, int snooze_index) {
    OrgEvent best;
    auto consider = [&](int kind, int index, int64_t at) {
        if (at <= from || at > to) return;
        if (best.kind == ORG_EV_NONE || at < best.at) {
            best.kind = kind;
            best.index = index;
            best.at = at;
        }
    };
    for (int i = 0; i < ORG_MAX_ALARMS; i++) {
        int64_t t = OrgAlarmNext(s.alarms[i], from);
        if (t) consider(ORG_EV_ALARM, i, t);
    }
    for (int i = 0; i < ORG_MAX_REMINDERS; i++) {
        if (s.rem[i].used) consider(ORG_EV_REMINDER, i, s.rem[i].at);
    }
    if (timer_end > 0) consider(ORG_EV_TIMER, -1, timer_end);
    if (snooze_until > 0) consider(ORG_EV_SNOOZE, snooze_index, snooze_until);
    return best;
}

// Ближайшее событие после after, или kind = NONE.
static inline OrgEvent OrgNextEvent(const OrgSchedule& s, int64_t after,
                                    int64_t timer_end, int64_t snooze_until, int snooze_index) {
    return OrgFindDue(s, after, after + 400LL * 24 * 3600, timer_end, snooze_until, snooze_index);
}

// Ближайший будильник и ближайшее напоминание (для строки на часах).
static inline int OrgNextAlarmIndex(const OrgSchedule& s, int64_t after, int64_t* at) {
    int best = -1;
    int64_t bt = 0;
    for (int i = 0; i < ORG_MAX_ALARMS; i++) {
        int64_t t = OrgAlarmNext(s.alarms[i], after);
        if (t && (best < 0 || t < bt)) { best = i; bt = t; }
    }
    if (at) *at = bt;
    return best;
}

static inline int OrgNextReminderIndex(const OrgSchedule& s, int64_t after, int64_t* at) {
    int best = -1;
    int64_t bt = 0;
    for (int i = 0; i < ORG_MAX_REMINDERS; i++) {
        if (!s.rem[i].used || s.rem[i].at <= after) continue;
        if (best < 0 || s.rem[i].at < bt) { best = i; bt = s.rem[i].at; }
    }
    if (at) *at = bt;
    return best;
}

// Убирает прошедшие одноразовые будильники и напоминания. true, если что-то убрали.
static inline bool OrgCleanup(OrgSchedule& s, int64_t now) {
    bool changed = false;
    for (int i = 0; i < ORG_MAX_ALARMS; i++) {
        OrgAlarm& a = s.alarms[i];
        if (a.used && a.days == 0 && a.once_at <= now) { a.used = 0; changed = true; }
    }
    for (int i = 0; i < ORG_MAX_REMINDERS; i++) {
        if (s.rem[i].used && s.rem[i].at <= now) { s.rem[i].used = 0; changed = true; }
    }
    return changed;
}

// ===== списки =====

// Находит пункт: по номеру (1, 2, ...) или по тексту без учёта регистра
// (сначала точное совпадение, потом по началу, потом по вхождению).
static inline int OrgListFind(const OrgList& l, const char* what) {
    // номер
    const char* p = what;
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        int n = atoi(p);
        const char* q = p;
        while (*q >= '0' && *q <= '9') q++;
        while (*q == ' ' || *q == '.') q++;
        if (*q == 0 && n >= 1 && n <= l.count) return n - 1;
    }
    std::string w = OrgLower(what);
    while (!w.empty() && w.back() == ' ') w.pop_back();
    while (!w.empty() && w.front() == ' ') w.erase(0, 1);
    if (w.empty()) return -1;
    for (int i = 0; i < l.count; i++) {
        if (OrgLower(l.items[i]) == w) return i;
    }
    for (int i = 0; i < l.count; i++) {
        if (OrgLower(l.items[i]).compare(0, w.size(), w) == 0) return i;
    }
    for (int i = 0; i < l.count; i++) {
        if (OrgLower(l.items[i]).find(w) != std::string::npos) return i;
    }
    return -1;
}

// Добавляет пункты, разделённые запятой или точкой с запятой.
// Повторы не добавляет. Возвращает, сколько добавлено.
static inline int OrgListAdd(OrgList& l, const char* items) {
    int added = 0;
    std::string all(items);
    size_t i = 0;
    while (i <= all.size()) {
        size_t j = all.find_first_of(",;\n", i);
        if (j == std::string::npos) j = all.size();
        std::string one = all.substr(i, j - i);
        char buf[ORG_ITEM_LEN];
        OrgCopyText(buf, sizeof(buf), one.c_str());
        if (buf[0] != 0 && l.count < ORG_LIST_MAX) {
            bool dup = false;
            std::string lb = OrgLower(buf);
            for (int k = 0; k < l.count; k++) {
                if (OrgLower(l.items[k]) == lb) { dup = true; break; }
            }
            if (!dup) {
                memcpy(l.items[l.count], buf, sizeof(buf));
                l.count++;
                added++;
            }
        }
        i = j + 1;
    }
    return added;
}

static inline void OrgListRemoveAt(OrgList& l, int idx) {
    if (idx < 0 || idx >= l.count) return;
    for (int k = idx; k + 1 < l.count; k++) memcpy(l.items[k], l.items[k + 1], ORG_ITEM_LEN);
    l.count--;
    memset(l.items[l.count], 0, ORG_ITEM_LEN);
}

// Список в JSON для ассистента: ["молоко","хлеб"].
static inline std::string OrgListJson(const OrgList& l) {
    std::string s = "[";
    for (int i = 0; i < l.count; i++) {
        if (i) s += ",";
        s += "\"";
        for (const char* p = l.items[i]; *p; p++) {
            if (*p == '"' || *p == '\\') s += '\\';
            s += *p;
        }
        s += "\"";
    }
    s += "]";
    return s;
}

#endif // __ORGANIZER_H__
