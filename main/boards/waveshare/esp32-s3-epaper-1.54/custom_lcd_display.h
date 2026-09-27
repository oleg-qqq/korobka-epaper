#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include "lcd_display.h"
#include "pixel_face.h"
#include "widget_art.h"
#include <mutex>
#include <esp_timer.h>
/* Display color */
typedef enum {
    DRIVER_COLOR_WHITE  = 0xff,
    DRIVER_COLOR_BLACK  = 0x00,
    FONT_BACKGROUND = DRIVER_COLOR_WHITE,
}COLOR_IMAGE;

enum class FaceMode {
    kShuttingDown,
    kBooting,
	kWidget, 
    kSpeaking,
    kListening,
    kEmotion,
    kIdle,
};

// Размеры историй для графиков.
#define DESK_ROOM_POINTS 144     // сутки по 10 минут
#define DESK_BATT_POINTS 168     // неделя по часу

// Всё, что нужно для экранов режима часов. Пустые точки графиков = NAN.
struct DeskScreenData {
    WeatherCache weather;       // погода на улице (weather.valid = false, если нет)
    bool room_ok = false;       // удалось ли прочитать датчик в комнате
    float room_t = 0.0f;
    float room_h = 0.0f;
    int battery = -1;           // заряд в процентах, -1 если неизвестен

    int64_t now = 0;            // время, для которого собраны данные

    // дом за сутки, от старых к новым, шаг 10 минут
    int64_t room_t0 = 0;        // время первой точки
    float room_t_hist[DESK_ROOM_POINTS];
    float room_h_hist[DESK_ROOM_POINTS];

    // улица по часам: прошедшие сутки и следующие сутки
    int64_t hour0 = 0;          // начало текущего часа
    float out_past[25];         // [24] = текущий час
    float out_next[25];         // [0] = текущий час
    float rain_next[24];        // вероятность дождя, [0] = текущий час

    // батарея за неделю, шаг час
    int64_t batt_t0 = 0;
    float batt_hist[DESK_BATT_POINTS];
    int batt_days_left = -1;    // примерно сколько дней осталось, -1 неизвестно

    // солнце сегодня
    bool sun_ok = false;
    int64_t sunrise = 0;
    int64_t sunset = 0;
    float uv_max = -1.0f;

    DeskScreenData();
};

// Страницы режима часов, листаются кнопкой BOOT по кругу.
enum DeskPage {
    DESK_PAGE_CLOCK = 0,    // часы, дата, дом и улица коротко
    DESK_PAGE_WEATHER,      // погода и прогноз на 3 дня
    DESK_PAGE_OUT24,        // улица: температура и дождь на сутки вперёд
    DESK_PAGE_ROOM,         // комната сейчас
    DESK_PAGE_ROOM24,       // комната: графики за сутки
    DESK_PAGE_BOTH,         // дом и улица за сутки, когда проветривать
    DESK_PAGE_SUN,          // восход, закат, УФ
    DESK_PAGE_MOON,         // фаза луны
    DESK_PAGE_BATTERY,      // батарея за неделю
    DESK_PAGE_COUNT
};

typedef struct {
    uint8_t cs;
    uint8_t dc;
    uint8_t rst;
    uint8_t busy;
    uint8_t mosi;
    uint8_t scl;
    int spi_host;
    int buffer_len;
}custom_lcd_spi_t;


class CustomLcdDisplay : public LcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy, custom_lcd_spi_t _lcd_spi_data,
                  bool quick_start = false);   // true: без LVGL и без очистки экрана (пробуждение из сна)
    ~CustomLcdDisplay();

    void EPD_Init();    /* e-paper init */
    void EPD_Clear();   /* clear screen */
    void EPD_Display(); /* flush buffer to e-paper */
    void ShowWeatherWidget(const WeatherCache& w, int seconds);
    void ShowRoomWidget(float temperature, float humidity, int seconds);

    /* режим часов (deep sleep) */
    // Рисует страницу и выводит её на экран. old_frame: что было на экране
    // до сна (для быстрого обновления без мигания), nullptr = полное обновление.
    // page_bar: показать внизу полоску "какая страница из скольких".
    void RenderDeskPage(int page, const DeskScreenData& d, const uint8_t* old_frame,
                        bool page_bar = false);
    void RenderFaceAfterWake(int face_type, const uint8_t* old_frame);
    void EPD_Sleep();   // перевести контроллер экрана в сон перед отключением питания
    const uint8_t* FrameBuffer() const { return buffer; }
    int FrameSize() const { return lcd_spi_data.buffer_len; }

    /* fast refresh */
    void EPD_DisplayPartBaseImage();
    void EPD_Init_Partial();
    void EPD_DisplayPart();
    void EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color);

    /* Pixel art face */
    void DrawFace(int face_type);   // целое лицо из kFaces (загрузка, выключение)
    void DrawFaceParts(int eyes, int mouth);   // составное лицо (загрузка, засыпание)
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetupUI() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;

    void SetShuttingDown(bool value) {
        face_mode_ = value ? FaceMode::kShuttingDown : FaceMode::kIdle;
        if (value) {
            if (widget_timer_) esp_timer_stop(widget_timer_);
            pending_widget_ = false;
        }
    }

    void SetBooting(bool value) {
        booting_ = value;
        if (face_mode_ == FaceMode::kShuttingDown) return;
        face_mode_ = value ? FaceMode::kBooting : FaceMode::kIdle;
    }

    FaceMode GetFaceMode() const { return face_mode_; }

    // Попросить аниматор перерисовать лицо на ближайшем шаге.
    void RequestFace() { force_redraw_ = true; }

    // Показать в простое мигающий значок слабого Wi-Fi.
    void PlayWifiWeak() { wifi_weak_request_ = true; }

    // Один кадр живого лица: целое лицо из kFaces или глаза + рот.
    struct AnimFrame {
        int16_t full;     // целое лицо из kFaces, или -1
        int8_t eyes;      // иначе составное: глаза
        int8_t mouth;     // и рот
        uint16_t ms;      // сколько держать
    };

private:
    int current_face_ = -1;
    const custom_lcd_spi_t lcd_spi_data;
    const int Width;
    const int Height;
    spi_device_handle_t spi;
    uint8_t *buffer = NULL;
    static void lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);
    void spi_gpio_init();
    void spi_port_init();
    void read_busy();
    volatile FaceMode face_mode_ = FaceMode::kIdle;
    volatile bool booting_ = false;
    esp_timer_handle_t widget_timer_ = nullptr;
    void HideWidget();
    void set_cs_1(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,1);}
    void set_cs_0(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,0);}
    void set_dc_1(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,1);}
    void set_dc_0(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,0);}
    void set_rst_1(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,1);}
    void set_rst_0(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,0);}

    void SPI_SendByte(uint8_t data);
    void EPD_SendData(uint8_t data);
    void EPD_SendCommand(uint8_t command);
    void writeBytes(uint8_t *buffer,int len);
    void writeBytes(const uint8_t *buffer, int len);
    void EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend);
    void EPD_SetCursor(uint16_t Xstart, uint16_t Ystart);
    void EPD_SetLut(const uint8_t *lut);
    void EPD_TurnOnDisplay();
    void EPD_TurnOnDisplayPart();

    // ===== Живое лицо: аниматор =====
    // Всё лицо рисует только аниматор, 4 раза в секунду он решает, что
    // показать. Экран защищён общей блокировкой, чтобы два рисования
    // никогда не шли одновременно.
    std::recursive_mutex draw_mutex_;
    esp_timer_handle_t anim_timer_ = nullptr;
    volatile bool force_redraw_ = true;
    volatile bool wifi_weak_request_ = false;
    volatile int emotion_ = 0;            // индекс в таблице эмоций, 0 = обычное
    FaceMode last_mode_ = FaceMode::kBooting;
    int64_t last_tick_ms_ = 0;
    int mode_ms_ = 0;                     // сколько мс в текущем режиме
    int next_event_ms_ = 3000;            // через сколько мс следующая случайная выходка
    AnimFrame seq_[8];                    // короткая сценка (моргнуть, зевнуть...)
    int seq_len_ = 0;
    int seq_pos_ = 0;
    int seq_left_ms_ = 0;
    int drawn_full_ = -2;
    int drawn_eyes_ = -1;
    int drawn_mouth_ = -1;
    // речь
    int speak_mouth_ = 0;
    int speak_mouth_left_ms_ = 0;
    int play_ms_ = 0;
    int speak_blink_ms_ = 4000;
    // слушает
    bool heard_voice_ = false;
    int silence_ms_ = 0;
    int think_ms_ = 0;
    // эмоция после речи
    int emotion_hold_ms_ = 0;

    void StartAnimator();
    void AnimTick();
    void StartSequence(const AnimFrame* frames, int count);
    void PickIdleEvent();
    void PickListenEvent();
    AnimFrame DecideFrame(int dt);
    void DrawAnimFrame(const AnimFrame& f);
    void PaintFaceParts(int eyes, int mouth);

    void DrawBitmap(const char* const* rows, int rows_count, int cols,
                    int x, int y, int scale);
    void DrawPixelText(const char* text, int x, int y, int scale);
    int  PixelTextWidth(const char* text, int scale);
    void DrawHLine(int x0, int x1, int y, int thickness);
    void DrawVLine(int x, int y0, int y1, int thickness);
    void DrawBorder(int thickness, bool dashed);
    void FillRect(int x0, int y0, int x1, int y1);
    void DrawTempRight(int t, int right, int y, int scale);
    void DrawBatteryIcon(int x, int y, int percent);
    void DrawWeekdayMarks(int x_right, int y, int wday);
    void DrawWeatherScreen(const WeatherCache& w);
    void DrawRoomScreen(float temperature, float humidity);
    void DrawClockScreen(const DeskScreenData& d);
    void DrawRoom24Screen(const DeskScreenData& d);
    void DrawOut24Screen(const DeskScreenData& d);
    void DrawBothScreen(const DeskScreenData& d);
    void DrawSunScreen(const DeskScreenData& d);
    void DrawMoonScreen(const DeskScreenData& d);
    void DrawBatteryScreen(const DeskScreenData& d);
    void DrawNoDataScreen(const char* const* icon);
    // графики
    void DrawLine(int x0, int y0, int x1, int y1, int thickness, bool dashed);
    void DrawDottedHLine(int x0, int x1, int y);
    void FillCircle(int cx, int cy, int r);
    void DrawRing(int cx, int cy, int r, int thickness);
    void DrawTextRight(const char* text, int right, int y, int scale);
    void DrawTextCenter(const char* text, int cx, int y, int scale);
    void DrawValueLabels(int lo, int hi, const char* unit, int y_top, int y_bottom);
    void PlotSeries(const float* v, int n, int64_t t_first, int step_sec,
                    int64_t win_t0, int win_span, int x0, int x1, int y0, int y1,
                    float vmin, float vmax, int thickness, bool dashed);
    void DrawTimeAxis(int64_t win_t0, int win_span, int x0, int x1, int y, int every_hours);
    void PaintFace(int face_type);
    void PresentAfterWake(const uint8_t* old_frame);
    void DrawPageBar(int page, int count);

    // виджеты по голосу
    void StartWidgetTimer(int seconds);
    int64_t widget_started_ms_ = 0;
    bool pending_widget_ = false;
    int pending_kind_ = 0;            // 1 погода, 2 комната
    WeatherCache pending_weather_;
    float pending_temp_ = 0.0f;
    float pending_hum_ = 0.0f;
    int pending_seconds_ = 0;
    void OnWidgetTimer();
};

#endif // __CUSTOM_LCD_DISPLAY_H__