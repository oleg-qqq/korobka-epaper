#include <stdio.h>
#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <vector>
#include <esp_log.h>
#include "custom_lcd_display.h"
#include "board.h"
#include "config.h"
#include "esp_lvgl_port.h"
#include "settings.h"
#include "pixel_face.h"
#include "widget_art.h"
#include <time.h>
#include <math.h>
#include <cmath>
#include <cstdlib>
#include "display/display.h"
#include "assets/lang_config.h"
#include "application.h"
#include <esp_random.h>

#define TAG "CustomLcdDisplay"
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT * BYTES_PER_PIXEL)

// ===== Настройки анимации и виджетов =====
static constexpr int kEmotionRevertMs = 1500;    // сколько держится эмоция после речи
// Минимальное время показа виджета. Если во время него прилетает
// второй виджет, он ждёт своей очереди, а не затирает первый мгновенно.
static constexpr int kMinWidgetMs = 5000;



const uint8_t WF_Full_1IN54[159] =
{											
    0x80,0x48,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x48,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x80,0x48,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x48,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0xA,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x8,0x1,0x0,0x8,0x1,0x0,0x2,				
    0xA,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,				
    0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,			
    0x22,0x17,0x41,0x0,0x32,0x20
};

const uint8_t WF_PARTIAL_1IN54_0[159] =
{
    0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0xF,0x0,0x0,0x0,0x0,0x0,0x0,
    0x1,0x1,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
    0x02,0x17,0x41,0xB0,0x32,0x28,
};

void CustomLcdDisplay::lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
    assert(disp != NULL);
    lv_disp_flush_ready(disp);
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, 
    int width, int height, int offset_x, int offset_y, 
    bool mirror_x, bool mirror_y, bool swap_xy, custom_lcd_spi_t _lcd_spi_data,
    bool quick_start) :
    LcdDisplay(panel_io, panel, width, height),
    lcd_spi_data(_lcd_spi_data),
    Width(width), Height(height) {

    ESP_LOGI(TAG, "Initialize SPI");
    spi_port_init();
    spi_gpio_init();

    if (quick_start) {
        // Пробуждение в режиме часов: LVGL не нужен, экран не очищаем,
        // чтобы картинка не мигала. Только буфер кадра.
        buffer = (uint8_t *) heap_caps_malloc(lcd_spi_data.buffer_len, MALLOC_CAP_SPIRAM);
        assert(buffer);
        EPD_Clear();
        return;
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    buffer = (uint8_t *) heap_caps_malloc(lcd_spi_data.buffer_len, MALLOC_CAP_SPIRAM);
    assert(buffer);
    display_ = lv_display_create(width, height); /* basic init with horizontal and vertical resolution in pixels */
    lv_display_set_flush_cb(display_, lvgl_flush_cb);
    lv_display_set_user_data(display_, this);

    uint8_t *buffer_1 = NULL;
    buffer_1          = (uint8_t *) heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    assert(buffer_1);
    lv_display_set_buffers(display_, buffer_1, NULL, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_FULL);

    ESP_LOGI(TAG, "EPD init");
    EPD_Init();
    EPD_Clear();
    EPD_Display();
    EPD_DisplayPartBaseImage();
    EPD_Init_Partial(); // partial refresh init

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

CustomLcdDisplay::~CustomLcdDisplay() {
    
}

void CustomLcdDisplay::spi_gpio_init() {
    int rst  = lcd_spi_data.rst;
    int cs   = lcd_spi_data.cs;
    int dc   = lcd_spi_data.dc;
    int busy = lcd_spi_data.busy;

    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst) | (0x1ULL << dc) | (0x1ULL << cs);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    gpio_conf.mode         = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (0x1ULL << busy);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    set_rst_1();
}

void CustomLcdDisplay::spi_port_init() {
    int              mosi     = lcd_spi_data.mosi;
    int              scl      = lcd_spi_data.scl;
    int              spi_host = lcd_spi_data.spi_host;
    esp_err_t        ret;
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num      = -1;
    buscfg.mosi_io_num      = mosi;
    buscfg.sclk_io_num      = scl;
    buscfg.quadwp_io_num    = -1;
    buscfg.quadhd_io_num    = -1;
    buscfg.max_transfer_sz  = Width * Height;

    spi_device_interface_config_t devcfg = {};
    devcfg.spics_io_num                  = -1;
    devcfg.clock_speed_hz                = 40 * 1000 * 1000; // Clock out at 10 MHz
    devcfg.mode                          = 0;                // SPI mode 0
    devcfg.queue_size                    = 7;                // We want to be able to queue 7 transactions at a time

    ret = spi_bus_initialize((spi_host_device_t) spi_host, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    ret = spi_bus_add_device((spi_host_device_t) spi_host, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);
}

void CustomLcdDisplay::read_busy() {
    int busy = lcd_spi_data.busy;
    while (gpio_get_level((gpio_num_t) busy) == 1) {
        vTaskDelay(pdMS_TO_TICKS(5)); // LOW: idle, HIGH: busy
    }
}

void CustomLcdDisplay::SPI_SendByte(uint8_t data) {
    esp_err_t         ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8;
    t.tx_buffer = &data;
    ret         = spi_device_polling_transmit(spi, &t); // Transmit!
    assert(ret == ESP_OK);                              // Should have had no issues.
}

void CustomLcdDisplay::EPD_SendData(uint8_t data) {
    set_dc_1();
    set_cs_0();
    SPI_SendByte(data);
    set_cs_1();
}

void CustomLcdDisplay::EPD_SendCommand(uint8_t command) {
    set_dc_0();
    set_cs_0();
    SPI_SendByte(command);
    set_cs_1();
}

void CustomLcdDisplay::writeBytes(uint8_t *buffer, int len) {
    set_dc_1();
    set_cs_0();
    esp_err_t         ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8 * len;
    t.tx_buffer = buffer;
    ret         = spi_device_polling_transmit(spi, &t); // Transmit!
    assert(ret == ESP_OK);
    set_cs_1();
}

void CustomLcdDisplay::writeBytes(const uint8_t *buffer, int len) {
    set_dc_1();
    set_cs_0();
    esp_err_t         ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8 * len;
    t.tx_buffer = buffer;
    ret         = spi_device_polling_transmit(spi, &t); // Transmit!
    assert(ret == ESP_OK);
    set_cs_1();
}

void CustomLcdDisplay::EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend) {
    EPD_SendCommand(0x44); // SET_RAM_X_ADDRESS_START_END_POSITION
    EPD_SendData((Xstart >> 3) & 0xFF);
    EPD_SendData((Xend >> 3) & 0xFF);

    EPD_SendCommand(0x45); // SET_RAM_Y_ADDRESS_START_END_POSITION
    EPD_SendData(Ystart & 0xFF);
    EPD_SendData((Ystart >> 8) & 0xFF);
    EPD_SendData(Yend & 0xFF);
    EPD_SendData((Yend >> 8) & 0xFF);
}

void CustomLcdDisplay::EPD_SetCursor(uint16_t Xstart, uint16_t Ystart) {
    EPD_SendCommand(0x4E); // SET_RAM_X_ADDRESS_COUNTER
    EPD_SendData(Xstart & 0xFF);

    EPD_SendCommand(0x4F); // SET_RAM_Y_ADDRESS_COUNTER
    EPD_SendData(Ystart & 0xFF);
    EPD_SendData((Ystart >> 8) & 0xFF);
}

void CustomLcdDisplay::EPD_SetLut(const uint8_t *lut) {
    EPD_SendCommand(0x32);
    writeBytes(lut, 153);
    read_busy();

    EPD_SendCommand(0x3f);
    EPD_SendData(lut[153]);

    EPD_SendCommand(0x03);
    EPD_SendData(lut[154]);

    EPD_SendCommand(0x04);
    EPD_SendData(lut[155]);
    EPD_SendData(lut[156]);
    EPD_SendData(lut[157]);

    EPD_SendCommand(0x2c);
    EPD_SendData(lut[158]);
}

void CustomLcdDisplay::EPD_TurnOnDisplay() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xc7);
    EPD_SendCommand(0x20);
    read_busy();
}

void CustomLcdDisplay::EPD_TurnOnDisplayPart() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xcf);
    EPD_SendCommand(0x20);
    read_busy();
}

void CustomLcdDisplay::EPD_Init() {
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(50));
    set_rst_0();
    vTaskDelay(pdMS_TO_TICKS(20));
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(50));

    read_busy();
    EPD_SendCommand(0x12); // SWRESET
    read_busy();

    EPD_SendCommand(0x01); // Driver output control
    EPD_SendData(0xC7);
    EPD_SendData(0x00);
    EPD_SendData(0x01);

    EPD_SendCommand(0x11); // data entry mode
    EPD_SendData(0x01);

    EPD_SetWindows(0, Width - 1, Height - 1, 0);

    EPD_SendCommand(0x3C); // BorderWavefrom
    EPD_SendData(0x01);

    EPD_SendCommand(0x18);
    EPD_SendData(0x80);

    EPD_SendCommand(0x22); // Load Temperature and waveform setting.
    EPD_SendData(0XB1);
    EPD_SendCommand(0x20);

    EPD_SetCursor(0, Height - 1);
    read_busy();

    EPD_SetLut(WF_Full_1IN54);
}

void CustomLcdDisplay::EPD_Clear() {
    int buffer_len = lcd_spi_data.buffer_len;
    memset(buffer, 0xff, buffer_len);
}

void CustomLcdDisplay::EPD_Display() {
    int buffer_len = lcd_spi_data.buffer_len;
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer, buffer_len);
    EPD_TurnOnDisplay();
}

void CustomLcdDisplay::EPD_DisplayPartBaseImage() {
    int buffer_len = lcd_spi_data.buffer_len;
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer, buffer_len);
    EPD_SendCommand(0x26);
    writeBytes(buffer, buffer_len);
    EPD_TurnOnDisplay();
}

void CustomLcdDisplay::EPD_Init_Partial() {
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(50));
    set_rst_0();
    vTaskDelay(pdMS_TO_TICKS(20));
    set_rst_1();
    vTaskDelay(pdMS_TO_TICKS(50));

    read_busy();

    EPD_SetLut(WF_PARTIAL_1IN54_0);

    EPD_SendCommand(0x37);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x40);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);

    EPD_SendCommand(0x3C); // BorderWavefrom
    EPD_SendData(0x80);

    EPD_SendCommand(0x22);
    EPD_SendData(0xc0);
    EPD_SendCommand(0x20);
    read_busy();
}

void CustomLcdDisplay::EPD_DisplayPart() {
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer, 5000);
    EPD_TurnOnDisplayPart();

    // Запоминаем показанный кадр как "старый". Без этого контроллер
    // считает разницу от устаревшего изображения, чёрные пиксели
    // никогда не стираются и кадры накладываются друг на друга.
    EPD_SendCommand(0x26);
    writeBytes(buffer, 5000);
}

void CustomLcdDisplay::EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color) {
    if (x >= Width || y >= Height) {
        ESP_LOGE("EPD", "Out of bounds pixel: (%d,%d)", x, y);
        return;
    }

    uint16_t index = y * 25 + (x >> 3);
    uint8_t  bit   = 7 - (x & 0x07);
    if (color == DRIVER_COLOR_WHITE) {
        buffer[index] |= (0x01 << bit);
    } else {
        buffer[index] &= ~(0x01 << bit);
    }
}

/* ==================== Pixel art face ==================== */

void CustomLcdDisplay::DrawFace(int face_type) {
    if (face_type < 0 || face_type >= FACE_COUNT) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(draw_mutex_);
    current_face_ = face_type;
    PaintFace(face_type);
    EPD_DisplayPart();
}

// Составное лицо сразу на экран. Для анимаций загрузки и засыпания,
// когда аниматор ещё не работает или уже остановлен.
void CustomLcdDisplay::DrawFaceParts(int eyes, int mouth) {
    std::lock_guard<std::recursive_mutex> lock(draw_mutex_);
    current_face_ = -1;
    PaintFaceParts(eyes, mouth);
    EPD_DisplayPart();
}

// Рисует лицо только в буфер, без вывода на экран.
void CustomLcdDisplay::PaintFace(int face_type) {
    if (face_type < 0 || face_type >= FACE_COUNT) {
        return;
    }
    const int cell = Width / FACE_GRID;   // размер одного "пикселя" лица
    const int offset = (Width - cell * FACE_GRID) / 2;

    EPD_Clear();

    for (int row = 0; row < FACE_GRID; row++) {
        const char* line = kFaces[face_type][row];
        if (line == nullptr) {
            continue;
        }
        for (int col = 0; col < FACE_GRID; col++) {
            if (line[col] != '#') {
                continue;
            }
            for (int dy = 0; dy < cell; dy++) {
                for (int dx = 0; dx < cell; dx++) {
                    EPD_DrawColorPixel(offset + col * cell + dx,
                                       offset + row * cell + dy,
                                       DRIVER_COLOR_BLACK);
                }
            }
        }
    }
}

//Графика виджетов
/* ==================== Графика виджетов ==================== */

static const PixelGlyph* FindGlyph(char ch) {
    for (int i = 0; i < kGlyphCount; i++) {
        if (kGlyphs[i].ch == ch) return &kGlyphs[i];
    }
    return nullptr;
}

static const char* const* WeatherIcon(int code) {
    if (code == 0)                   return ICON_SUN;
    if (code == 1 || code == 2)      return ICON_PARTLY;
    if (code == 3)                   return ICON_CLOUD;
    if (code == 45 || code == 48)    return ICON_FOG;
    if (code >= 71 && code <= 77)    return ICON_SNOW;
    if (code == 85 || code == 86)    return ICON_SNOW;
    if (code >= 95)                  return ICON_STORM;
    if (code >= 51)                  return ICON_RAIN;
    return ICON_CLOUD;
}

static int RoundTemp(float t) {
    return (int)(t + (t < 0.0f ? -0.5f : 0.5f));
}

void CustomLcdDisplay::DrawBitmap(const char* const* rows, int rows_count, int cols,
                                  int x, int y, int scale) {
    for (int r = 0; r < rows_count; r++) {
        const char* line = rows[r];
        if (line == nullptr) continue;
        for (int c = 0; c < cols; c++) {
            if (line[c] != '#') continue;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    EPD_DrawColorPixel(x + c * scale + dx, y + r * scale + dy, DRIVER_COLOR_BLACK);
                }
            }
        }
    }
}

int CustomLcdDisplay::PixelTextWidth(const char* text, int scale) {
    int n = 0;
    for (const char* p = text; *p != '\0'; p++) n++;
    if (n == 0) return 0;
    return n * GLYPH_W * scale + (n - 1) * scale;
}

void CustomLcdDisplay::DrawPixelText(const char* text, int x, int y, int scale) {
    int cx = x;
    for (const char* p = text; *p != '\0'; p++) {
        const PixelGlyph* g = FindGlyph(*p);
        if (g != nullptr) {
            DrawBitmap(g->rows, GLYPH_H, GLYPH_W, cx, y, scale);
        }
        cx += (GLYPH_W + 1) * scale;
    }
}

void CustomLcdDisplay::DrawHLine(int x0, int x1, int y, int thickness) {
    for (int yy = y; yy < y + thickness; yy++) {
        for (int x = x0; x <= x1; x++) {
            EPD_DrawColorPixel(x, yy, DRIVER_COLOR_BLACK);
        }
    }
}

void CustomLcdDisplay::DrawVLine(int x, int y0, int y1, int thickness) {
    for (int xx = x; xx < x + thickness; xx++) {
        for (int y = y0; y <= y1; y++) {
            EPD_DrawColorPixel(xx, y, DRIVER_COLOR_BLACK);
        }
    }
}

void CustomLcdDisplay::DrawBorder(int thickness, bool dashed) {
    // Пунктир: четыре точки закрашены, три пропущены.
    for (int i = 0; i < thickness; i++) {
        for (int x = 0; x < Width; x++) {
            if (dashed && (x % 7) >= 4) continue;
            EPD_DrawColorPixel(x, i, DRIVER_COLOR_BLACK);
            EPD_DrawColorPixel(x, Height - 1 - i, DRIVER_COLOR_BLACK);
        }
        for (int y = 0; y < Height; y++) {
            if (dashed && (y % 7) >= 4) continue;
            EPD_DrawColorPixel(i, y, DRIVER_COLOR_BLACK);
            EPD_DrawColorPixel(Width - 1 - i, y, DRIVER_COLOR_BLACK);
        }
    }
}

// ===== Общие мелочи для экранов =====

static bool IsTimeValid(time_t t) {
    return t > 1600000000;   // позже сентября 2020, значит время уже пришло из сети
}

// Данные старше этого считаются несвежими, рамка становится пунктирной.
// Чуть больше суток, потому что в режиме часов погода обновляется раз в сутки.
static constexpr int64_t kWeatherStaleSec = 25LL * 60 * 60;

static bool IsWeatherStale(const WeatherCache& w) {
    if (!w.valid) return true;
    if (w.fetched_at_unix <= 0) return false;   // время получения неизвестно, данные свежие из этой сессии
    time_t now = time(nullptr);
    if (!IsTimeValid(now)) return false;
    return (now - w.fetched_at_unix) > kWeatherStaleSec;
}

// Лицо-настроение комнаты (описано ниже, рядом с графиками).
static const char* const* RoomMoodIcon(float t, float h);

void CustomLcdDisplay::FillRect(int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            EPD_DrawColorPixel(x, y, DRIVER_COLOR_BLACK);
        }
    }
}

// Температура с кружком градуса, выровненная по правому краю.
void CustomLcdDisplay::DrawTempRight(int t, int right, int y, int scale) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", t);
    int tw = PixelTextWidth(buf, scale);
    int ow = PixelTextWidth("o", 2);
    int gx = right - (tw + 3 + ow);
    DrawPixelText(buf, gx, y, scale);
    DrawPixelText("o", gx + tw + 3, y, 2);
}

// Батарейка 24x12 с пупырышком справа, заливка по проценту.
void CustomLcdDisplay::DrawBatteryIcon(int x, int y, int percent) {
    if (percent < 0) return;
    if (percent > 100) percent = 100;
    DrawHLine(x, x + 23, y, 1);
    DrawHLine(x, x + 23, y + 11, 1);
    DrawVLine(x, y, y + 11, 1);
    DrawVLine(x + 23, y, y + 11, 1);
    FillRect(x + 24, y + 3, x + 26, y + 8);
    const int inner = 20;
    int fill = inner * percent / 100;
    if (percent > 0 && fill == 0) fill = 1;
    if (fill > 0) {
        FillRect(x + 2, y + 2, x + 1 + fill, y + 9);
    }
}

// Семь меток дней недели, с понедельника. Сегодня закрашенный квадратик,
// остальные дни маленькие точки.
void CustomLcdDisplay::DrawWeekdayMarks(int x_right, int y, int wday) {
    const int size = 5, gap = 3;
    const int total = 7 * size + 6 * gap;
    const int x0 = x_right - total + 1;
    const int today = (wday + 6) % 7;   // tm_wday: 0 = воскресенье
    for (int i = 0; i < 7; i++) {
        int xx = x0 + i * (size + gap);
        if (i == today) {
            FillRect(xx, y, xx + size - 1, y + size - 1);
        } else {
            EPD_DrawColorPixel(xx + 2, y + 2, DRIVER_COLOR_BLACK);
        }
    }
}

// ===== Экран погоды (только рисование в буфер) =====

void CustomLcdDisplay::DrawWeatherScreen(const WeatherCache& w) {
    // рамка: сплошная на свежих данных, пунктирная если им больше суток
    DrawBorder(2, IsWeatherStale(w));

    char buf[16];

    // верх слева: время, если оно уже пришло из сети
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (IsTimeValid(now)) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        DrawPixelText(buf, 12, 12, 2);
    }

    if (!w.valid) {
        // погоды ещё ни разу не было: прочерк по центру
        DrawHLine(12, 187, 32, 1);
        int dw = PixelTextWidth("--", 5);
        DrawPixelText("--", (Width - dw) / 2, 82, 5);
        return;
    }

    // верх справа: капля и влажность
    snprintf(buf, sizeof(buf), "%d", w.humidity);
    int hum_w = PixelTextWidth(buf, 2);
    DrawBitmap(ICON_DROP, SMALL_ICON_SIZE, SMALL_ICON_SIZE, 188 - 16 - 6 - hum_w, 11, 2);
    DrawPixelText(buf, 188 - hum_w, 12, 2);

    DrawHLine(12, 187, 32, 1);

    // центр: значок слева, температура справа
    DrawBitmap(WeatherIcon(w.weather_code), ICON_SIZE, ICON_SIZE, 14, 46, 4);

    snprintf(buf, sizeof(buf), "%d", RoundTemp(w.temperature));
    int t_w = PixelTextWidth(buf, 5);
    int deg_w = PixelTextWidth("o", 2);
    int gx = 186 - (t_w + 4 + deg_w);
    DrawPixelText(buf, gx, 52, 5);
    DrawPixelText("o", gx + t_w + 4, 52, 2);

    DrawHLine(12, 187, 124, 1);

    // низ: прогноз на три дня
    if (w.has_days) {
        const int centers[3] = { 46, 100, 154 };
        for (int i = 0; i < 3; i++) {
            snprintf(buf, sizeof(buf), "%d", w.days[i].day);
            int dw = PixelTextWidth(buf, 2);
            DrawPixelText(buf, centers[i] - dw / 2, 130, 2);

            DrawBitmap(WeatherIcon(w.days[i].code), ICON_SIZE, ICON_SIZE,
                       centers[i] - 16, 146, 2);

            snprintf(buf, sizeof(buf), "%d", RoundTemp(w.days[i].tmax));
            int tw2 = PixelTextWidth(buf, 2);
            DrawPixelText(buf, centers[i] - tw2 / 2, 178, 2);
        }
    }
}

// ===== Экран комнаты (только рисование в буфер) =====

void CustomLcdDisplay::DrawRoomScreen(float temperature, float humidity) {
    DrawBorder(2, false);

    char buf[16];

    // шапка: домик слева, часы справа
    DrawBitmap(ICON_HOUSE, ICON_SIZE, ICON_SIZE, 12, 8, 2);

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (IsTimeValid(now)) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        DrawPixelText(buf, 188 - PixelTextWidth(buf, 2), 16, 2);
    }

    DrawHLine(12, 187, 46, 1);

    // центр: термометр и крупная температура
    DrawBitmap(ICON_THERMO, ICON_SIZE, ICON_SIZE, 14, 54, 3);

    snprintf(buf, sizeof(buf), "%d", RoundTemp(temperature));
    int t_w = PixelTextWidth(buf, 5);
    int deg_w = PixelTextWidth("o", 2);
    int gx = 186 - (t_w + 4 + deg_w);
    DrawPixelText(buf, gx, 58, 5);
    DrawPixelText("o", gx + t_w + 4, 58, 2);

    DrawHLine(12, 187, 116, 1);

    // низ: капля, влажность, полоса заполнения
    DrawBitmap(ICON_DROP_BIG, ICON_SIZE, ICON_SIZE, 14, 124, 2);

    int hum = RoundTemp(humidity);
    if (hum < 0) hum = 0;
    if (hum > 100) hum = 100;
    snprintf(buf, sizeof(buf), "%d", hum);
    int h_w = PixelTextWidth(buf, 4);
    DrawPixelText(buf, 60, 128, 4);
    DrawPixelText("%", 60 + h_w + 4, 138, 2);

    const int bx0 = 14, bx1 = 186, by0 = 168, by1 = 184;
    DrawHLine(bx0, bx1, by0, 1);
    DrawHLine(bx0, bx1, by1, 1);
    DrawVLine(bx0, by0, by1, 1);
    DrawVLine(bx1, by0, by1, 1);

    int inner = bx1 - bx0 - 4;
    int fill = inner * hum / 100;
    for (int y = by0 + 4; y <= by1 - 4; y++) {
        for (int x = bx0 + 2; x < bx0 + 2 + fill; x++) {
            EPD_DrawColorPixel(x, y, DRIVER_COLOR_BLACK);
        }
    }
}

// ===== Экран часов (главная страница режима часов) =====

void CustomLcdDisplay::DrawClockScreen(const DeskScreenData& d) {
    const WeatherCache& w = d.weather;
    DrawBorder(2, IsWeatherStale(w));

    char buf[16];
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    bool time_ok = IsTimeValid(now);

    // шапка: дата, дни недели, батарейка
    if (time_ok) {
        snprintf(buf, sizeof(buf), "%02d.%02d", tm_now.tm_mday, tm_now.tm_mon + 1);
    } else {
        snprintf(buf, sizeof(buf), "--.--");
    }
    DrawPixelText(buf, 12, 12, 2);
    if (time_ok) {
        DrawWeekdayMarks(150, 18, tm_now.tm_wday);
    }
    DrawBatteryIcon(160, 13, d.battery);

    DrawHLine(12, 187, 32, 1);

    // центр: крупные часы
    if (time_ok) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    int tw = PixelTextWidth(buf, 6);
    DrawPixelText(buf, (Width - tw) / 2, 57, 6);

    DrawHLine(12, 187, 124, 1);
    DrawVLine(100, 130, 186, 1);

    // низ слева: дома. Вместо домика лицо-настроение комнаты.
    DrawBitmap(d.room_ok ? RoomMoodIcon(d.room_t, d.room_h) : ICON_HOUSE,
               ICON_SIZE, ICON_SIZE, 10, 130, 2);
    if (d.room_ok) {
        DrawTempRight(RoundTemp(d.room_t), 94, 136, 3);
        int hum = RoundTemp(d.room_h);
        if (hum < 0) hum = 0;
        if (hum > 100) hum = 100;
        snprintf(buf, sizeof(buf), "%d%%", hum);
        DrawBitmap(ICON_DROP, SMALL_ICON_SIZE, SMALL_ICON_SIZE, 12, 169, 2);
        DrawPixelText(buf, 94 - PixelTextWidth(buf, 2), 170, 2);
    } else {
        DrawPixelText("--", 94 - PixelTextWidth("--", 3), 136, 3);
    }

    // низ справа: на улице
    if (w.valid) {
        DrawBitmap(WeatherIcon(w.weather_code), ICON_SIZE, ICON_SIZE, 104, 130, 2);
        DrawTempRight(RoundTemp(w.temperature), 188, 136, 3);
        snprintf(buf, sizeof(buf), "%d%%", w.humidity);
        DrawBitmap(ICON_DROP, SMALL_ICON_SIZE, SMALL_ICON_SIZE, 106, 169, 2);
        DrawPixelText(buf, 188 - PixelTextWidth(buf, 2), 170, 2);
    } else {
        DrawPixelText("--", 188 - PixelTextWidth("--", 3), 136, 3);
    }
}

// ===== Графики и новые страницы режима часов =====

DeskScreenData::DeskScreenData() {
    for (int i = 0; i < DESK_ROOM_POINTS; i++) {
        room_t_hist[i] = NAN;
        room_h_hist[i] = NAN;
    }
    for (int i = 0; i < 25; i++) {
        out_past[i] = NAN;
        out_next[i] = NAN;
    }
    for (int i = 0; i < 24; i++) {
        rain_next[i] = NAN;
    }
    for (int i = 0; i < DESK_BATT_POINTS; i++) {
        batt_hist[i] = NAN;
    }
}

// Пороги для лица-настроения комнаты.
static constexpr float kMoodHotC = 27.0f;
static constexpr float kMoodColdC = 18.0f;
static constexpr float kMoodHumidPct = 65.0f;
static constexpr float kMoodDryPct = 35.0f;

static const char* const* RoomMoodIcon(float t, float h) {
    if (t > kMoodHotC)     return ICON_MOOD_HOT;
    if (h > kMoodHumidPct) return ICON_MOOD_HUMID;
    if (t < kMoodColdC)    return ICON_MOOD_COLD;
    if (h < kMoodDryPct)   return ICON_MOOD_DRY;
    return ICON_MOOD_OK;
}

// Минимум и максимум ряда, округлённые наружу, с минимальным размахом,
// чтобы ровный день не растягивался на весь график.
static bool SeriesRange(const float* a, int na, const float* b, int nb,
                        float min_span, int& lo, int& hi) {
    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < na; i++) {
        if (std::isnan(a[i])) continue;
        if (a[i] < mn) mn = a[i];
        if (a[i] > mx) mx = a[i];
    }
    for (int i = 0; i < nb; i++) {
        if (std::isnan(b[i])) continue;
        if (b[i] < mn) mn = b[i];
        if (b[i] > mx) mx = b[i];
    }
    if (mn > mx) return false;   // ни одной точки
    lo = (int)floorf(mn);
    hi = (int)ceilf(mx);
    if (hi - lo < min_span) {
        float mid = (mn + mx) / 2.0f;
        lo = (int)floorf(mid - min_span / 2.0f);
        hi = (int)ceilf(mid + min_span / 2.0f);
    }
    return true;
}

// Значение ряда в момент t (линейно между точками), NAN если нет данных.
static float SampleAt(const float* v, int n, int64_t t_first, int step, int64_t t) {
    if (t < t_first) return NAN;
    int64_t off = t - t_first;
    int i = (int)(off / step);
    if (i >= n) return NAN;
    if (i == n - 1) return v[i];
    float a = v[i], b = v[i + 1];
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) ? b : a;
    float f = (float)(off - (int64_t)i * step) / (float)step;
    return a + (b - a) * f;
}

void CustomLcdDisplay::DrawLine(int x0, int y0, int x1, int y1, int thickness, bool dashed) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int n = 0;
    while (true) {
        if (!dashed || ((n / 2) % 2) == 0) {
            for (int t = 0; t < thickness; t++) {
                if (x0 >= 0 && x0 < Width && y0 + t >= 0 && y0 + t < Height) {
                    EPD_DrawColorPixel(x0, y0 + t, DRIVER_COLOR_BLACK);
                }
            }
        }
        n++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void CustomLcdDisplay::DrawDottedHLine(int x0, int x1, int y) {
    for (int x = x0; x <= x1; x += 3) {
        EPD_DrawColorPixel(x, y, DRIVER_COLOR_BLACK);
    }
}

void CustomLcdDisplay::FillCircle(int cx, int cy, int r) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y > r * r) continue;
            int px = cx + x, py = cy + y;
            if (px >= 0 && px < Width && py >= 0 && py < Height) {
                EPD_DrawColorPixel(px, py, DRIVER_COLOR_BLACK);
            }
        }
    }
}

void CustomLcdDisplay::DrawRing(int cx, int cy, int r, int thickness) {
    float outer = (r + 0.5f) * (r + 0.5f);
    float inner = (r - thickness + 0.5f) * (r - thickness + 0.5f);
    for (int y = -r - 1; y <= r + 1; y++) {
        for (int x = -r - 1; x <= r + 1; x++) {
            float d = (float)(x * x + y * y);
            if (d < inner || d > outer) continue;
            int px = cx + x, py = cy + y;
            if (px >= 0 && px < Width && py >= 0 && py < Height) {
                EPD_DrawColorPixel(px, py, DRIVER_COLOR_BLACK);
            }
        }
    }
}

void CustomLcdDisplay::DrawTextRight(const char* text, int right, int y, int scale) {
    DrawPixelText(text, right - PixelTextWidth(text, scale), y, scale);
}

void CustomLcdDisplay::DrawTextCenter(const char* text, int cx, int y, int scale) {
    DrawPixelText(text, cx - PixelTextWidth(text, scale) / 2, y, scale);
}

// Подписи максимума и минимума слева от графика и пунктир на их уровне.
void CustomLcdDisplay::DrawValueLabels(int lo, int hi, const char* unit, int y_top, int y_bottom) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%s", hi, unit);
    DrawTextRight(buf, 27, y_top - 2, 1);
    snprintf(buf, sizeof(buf), "%d%s", lo, unit);
    DrawTextRight(buf, 27, y_bottom - 6, 1);
    DrawDottedHLine(30, 186, y_top);
    DrawDottedHLine(30, 186, y_bottom);
}

// Рисует ряд значений v (шаг step_sec, первая точка в t_first) в окне
// времени [win_t0, win_t0 + win_span] на прямоугольнике x0..x1, y0..y1.
void CustomLcdDisplay::PlotSeries(const float* v, int n, int64_t t_first, int step_sec,
                                  int64_t win_t0, int win_span, int x0, int x1, int y0, int y1,
                                  float vmin, float vmax, int thickness, bool dashed) {
    float range = vmax - vmin;
    if (range < 0.001f) range = 0.001f;
    bool have_prev = false;
    int px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        int64_t t = t_first + (int64_t)i * step_sec;
        if (std::isnan(v[i]) || t < win_t0 || t > win_t0 + win_span) {
            have_prev = false;
            continue;
        }
        int x = x0 + (int)((t - win_t0) * (x1 - x0) / win_span);
        int y = y1 - (int)lroundf((v[i] - vmin) * (y1 - y0) / range);
        if (have_prev) {
            DrawLine(px, py, x, y, thickness, dashed);
        } else {
            // Точка без соседа слева. Если и справа соседа нет, линии не будет,
            // поэтому рисуем её квадратиком, чтобы одиночный замер был виден.
            bool next_ok = (i + 1 < n) && !std::isnan(v[i + 1]) &&
                           (t + step_sec <= win_t0 + win_span);
            if (!next_ok) {
                FillRect(x - 1, y - 1, x + 1, y + thickness);
            }
        }
        px = x;
        py = y;
        have_prev = true;
    }
}

// Ось времени: засечки и подписи часов, кратных every_hours.
void CustomLcdDisplay::DrawTimeAxis(int64_t win_t0, int win_span, int x0, int x1, int y, int every_hours) {
    int64_t first_hour = (win_t0 / 3600 + 1) * 3600;
    for (int64_t t = first_hour; t <= win_t0 + win_span; t += 3600) {
        time_t tt = (time_t)t;
        struct tm tm_h;
        localtime_r(&tt, &tm_h);
        if (tm_h.tm_hour % every_hours != 0) continue;
        int x = x0 + (int)((t - win_t0) * (x1 - x0) / win_span);
        DrawVLine(x, y, y + 2, 1);
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", tm_h.tm_hour);
        DrawTextCenter(buf, x, y + 5, 1);
    }
}

// Нет данных для страницы: значок и прочерк.
void CustomLcdDisplay::DrawNoDataScreen(const char* const* icon) {
    DrawBorder(2, true);
    DrawBitmap(icon, ICON_SIZE, ICON_SIZE, (Width - ICON_SIZE * 4) / 2, 40, 4);
    DrawTextCenter("--", Width / 2, 130, 5);
}

// ----- Дом за сутки -----
void CustomLcdDisplay::DrawRoom24Screen(const DeskScreenData& d) {
    int tlo, thi, hlo, hhi;
    bool have_t = SeriesRange(d.room_t_hist, DESK_ROOM_POINTS, nullptr, 0, 3.0f, tlo, thi);
    bool have_h = SeriesRange(d.room_h_hist, DESK_ROOM_POINTS, nullptr, 0, 10.0f, hlo, hhi);
    if (!have_t && !have_h) {
        DrawNoDataScreen(ICON_HOUSE);
        return;
    }
    DrawBorder(2, false);
    char buf[16];
    DrawBitmap(ICON_HOUSE, ICON_SIZE, ICON_SIZE, 10, 8, 1);
    if (d.room_ok) {
        DrawTempRight(RoundTemp(d.room_t), 120, 9, 2);
        DrawBitmap(ICON_DROP, SMALL_ICON_SIZE, SMALL_ICON_SIZE, 132, 11, 1);
        snprintf(buf, sizeof(buf), "%d%%", RoundTemp(d.room_h));
        DrawTextRight(buf, 188, 10, 2);
    }
    DrawHLine(12, 187, 30, 1);

    const int64_t win_t0 = d.now - 24 * 3600;
    const int span = 24 * 3600;
    if (have_t) {
        DrawValueLabels(tlo, thi, "o", 38, 102);
        PlotSeries(d.room_t_hist, DESK_ROOM_POINTS, d.room_t0, 600, win_t0, span,
                   30, 186, 38, 102, (float)tlo, (float)thi, 2, false);
    }
    if (have_h) {
        DrawValueLabels(hlo, hhi, "%", 112, 162);
        PlotSeries(d.room_h_hist, DESK_ROOM_POINTS, d.room_t0, 600, win_t0, span,
                   30, 186, 112, 162, (float)hlo, (float)hhi, 2, false);
    }
    DrawTimeAxis(win_t0, span, 30, 186, 170, 6);
}

// ----- Улица на сутки вперёд -----
void CustomLcdDisplay::DrawOut24Screen(const DeskScreenData& d) {
    int lo, hi;
    if (!SeriesRange(d.out_next, 25, nullptr, 0, 4.0f, lo, hi)) {
        DrawNoDataScreen(ICON_PARTLY);
        return;
    }
    DrawBorder(2, IsWeatherStale(d.weather));
    char buf[16];
    const WeatherCache& w = d.weather;
    DrawBitmap(WeatherIcon(w.valid ? w.weather_code : 3), ICON_SIZE, ICON_SIZE, 10, 8, 1);
    float now_t = !std::isnan(d.out_next[0]) ? d.out_next[0] : w.temperature;
    DrawTempRight(RoundTemp(now_t), 120, 9, 2);
    int rain_max = -1;
    for (int i = 0; i < 24; i++) {
        if (!std::isnan(d.rain_next[i]) && (int)d.rain_next[i] > rain_max) rain_max = (int)d.rain_next[i];
    }
    if (rain_max >= 0) {
        DrawBitmap(ICON_UMBRELLA, ICON_SIZE, ICON_SIZE, 130, 8, 1);
        snprintf(buf, sizeof(buf), "%d%%", rain_max);
        DrawTextRight(buf, 188, 10, 2);
    }
    DrawHLine(12, 187, 30, 1);

    const int64_t win_t0 = d.hour0;
    const int span = 24 * 3600;
    DrawValueLabels(lo, hi, "o", 38, 124);
    PlotSeries(d.out_next, 25, d.hour0, 3600, win_t0, span, 30, 186, 38, 124,
               (float)lo, (float)hi, 2, false);
    // точка "сейчас"
    if (!std::isnan(d.out_next[0])) {
        int y_now = 124 - (int)lroundf((d.out_next[0] - lo) * (124 - 38) / (float)(hi - lo));
        FillCircle(30, y_now + 1, 3);
    }
    // столбики вероятности дождя
    const int base = 160;
    if (rain_max >= 0) {
        for (int i = 0; i < 24; i++) {
            if (std::isnan(d.rain_next[i])) continue;
            int x = 31 + i * (186 - 31) / 24;
            int hgt = (int)lroundf(d.rain_next[i] * 26.0f / 100.0f);
            if (hgt > 0) FillRect(x, base - hgt, x + 4, base);
        }
        DrawBitmap(ICON_DROP, SMALL_ICON_SIZE, SMALL_ICON_SIZE, 8, 146, 1);
    }
    DrawHLine(30, 186, base + 1, 1);
    DrawTimeAxis(win_t0, span, 30, 186, 166, 6);
}

// ----- Дом и улица за сутки: когда проветривать -----
void CustomLcdDisplay::DrawBothScreen(const DeskScreenData& d) {
    int lo, hi;
    if (!SeriesRange(d.room_t_hist, DESK_ROOM_POINTS, d.out_past, 25, 4.0f, lo, hi)) {
        DrawNoDataScreen(ICON_HOUSE);
        return;
    }
    DrawBorder(2, false);
    // шапка-легенда: значок, образец линии, температура сейчас
    DrawBitmap(ICON_HOUSE, ICON_SIZE, ICON_SIZE, 10, 8, 1);
    DrawLine(30, 15, 44, 15, 2, false);
    if (d.room_ok) DrawTempRight(RoundTemp(d.room_t), 94, 9, 2);
    const WeatherCache& w = d.weather;
    DrawBitmap(WeatherIcon(w.valid ? w.weather_code : 3), ICON_SIZE, ICON_SIZE, 104, 8, 1);
    DrawLine(124, 15, 138, 15, 2, true);
    if (!std::isnan(d.out_past[24])) DrawTempRight(RoundTemp(d.out_past[24]), 188, 9, 2);
    else if (w.valid) DrawTempRight(RoundTemp(w.temperature), 188, 9, 2);
    DrawHLine(12, 187, 30, 1);

    const int64_t win_t0 = d.now - 24 * 3600;
    const int span = 24 * 3600;
    const int x0 = 30, x1 = 186, y0 = 38, y1 = 156;
    DrawValueLabels(lo, hi, "o", y0, y1);

    // Штриховка там, где на улице прохладнее, чем дома: время проветривать.
    const int64_t out_t0 = d.hour0 - 24 * 3600;
    const float range = (float)(hi - lo);
    for (int x = x0; x <= x1; x++) {
        int64_t t = win_t0 + (int64_t)(x - x0) * span / (x1 - x0);
        float home = SampleAt(d.room_t_hist, DESK_ROOM_POINTS, d.room_t0, 600, t);
        float out = SampleAt(d.out_past, 25, out_t0, 3600, t);
        if (std::isnan(home) || std::isnan(out) || out >= home) continue;
        int yh = y1 - (int)lroundf((home - lo) * (y1 - y0) / range);
        int yo = y1 - (int)lroundf((out - lo) * (y1 - y0) / range);
        for (int y = yh + 2; y < yo; y++) {
            if (((x + y) % 4) == 0) EPD_DrawColorPixel(x, y, DRIVER_COLOR_BLACK);
        }
    }

    PlotSeries(d.room_t_hist, DESK_ROOM_POINTS, d.room_t0, 600, win_t0, span,
               x0, x1, y0, y1, (float)lo, (float)hi, 2, false);
    PlotSeries(d.out_past, 25, out_t0, 3600, win_t0, span,
               x0, x1, y0, y1, (float)lo, (float)hi, 2, true);
    DrawTimeAxis(win_t0, span, x0, x1, 166, 6);
}

// ----- Солнце -----
void CustomLcdDisplay::DrawSunScreen(const DeskScreenData& d) {
    if (!d.sun_ok || !IsTimeValid((time_t)d.now)) {
        DrawNoDataScreen(ICON_SUNRISE);
        return;
    }
    DrawBorder(2, false);
    char buf[16];

    // дуга пути солнца
    const int cx = 100, cy = 96, rx = 82, ry = 70;
    for (int a = 0; a <= 180; a += 3) {
        float t = a * (float)M_PI / 180.0f;
        EPD_DrawColorPixel(cx - (int)lroundf(rx * cosf(t)), cy - (int)lroundf(ry * sinf(t)), DRIVER_COLOR_BLACK);
    }
    DrawHLine(12, 187, cy, 1);
    // солнце на дуге, только днём
    if (d.now > d.sunrise && d.now < d.sunset) {
        float frac = (float)(d.now - d.sunrise) / (float)(d.sunset - d.sunrise);
        float t = (float)M_PI * frac;
        int sx = cx - (int)lroundf(rx * cosf(t));
        int sy = cy - (int)lroundf(ry * sinf(t));
        FillCircle(sx, sy, 7);
        for (int k = 0; k < 8; k++) {
            float a = k * (float)M_PI / 4.0f;
            for (int rr = 10; rr <= 11; rr++) {
                int px = sx + (int)lroundf(rr * cosf(a));
                int py = sy + (int)lroundf(rr * sinf(a));
                if (px >= 0 && px < Width && py >= 0 && py < Height) {
                    EPD_DrawColorPixel(px, py, DRIVER_COLOR_BLACK);
                }
            }
        }
    }

    // восход и закат
    struct tm tm_r, tm_s;
    time_t tr = (time_t)d.sunrise, ts = (time_t)d.sunset;
    localtime_r(&tr, &tm_r);
    localtime_r(&ts, &tm_s);
    DrawBitmap(ICON_SUNRISE, ICON_SIZE, ICON_SIZE, 12, 104, 1);
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_r.tm_hour, tm_r.tm_min);
    DrawPixelText(buf, 32, 106, 2);
    DrawBitmap(ICON_SUNSET, ICON_SIZE, ICON_SIZE, 110, 104, 1);
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_s.tm_hour, tm_s.tm_min);
    DrawTextRight(buf, 188, 106, 2);
    DrawHLine(12, 187, 128, 1);

    // долгота дня
    int len_min = (int)((d.sunset - d.sunrise) / 60);
    DrawBitmap(ICON_HOURGLASS, ICON_SIZE, ICON_SIZE, 12, 136, 1);
    snprintf(buf, sizeof(buf), "%02d:%02d", len_min / 60, len_min % 60);
    DrawPixelText(buf, 32, 138, 2);

    // УФ-индекс и шкала из 11 делений
    if (d.uv_max >= 0.0f) {
        int uv = (int)lroundf(d.uv_max);
        DrawPixelText("UV", 122, 138, 2);
        snprintf(buf, sizeof(buf), "%d", uv);
        DrawTextRight(buf, 188, 138, 2);
        for (int i = 0; i < 11; i++) {
            int x = 14 + i * 16;
            if (i < uv) {
                FillRect(x, 164, x + 12, 182);
            } else {
                DrawHLine(x, x + 12, 164, 1);
                DrawHLine(x, x + 12, 182, 1);
                DrawVLine(x, 164, 182, 1);
                DrawVLine(x + 12, 164, 182, 1);
            }
        }
    }
}

// ----- Луна -----
// Фаза считается по времени, интернет не нужен.
void CustomLcdDisplay::DrawMoonScreen(const DeskScreenData& d) {
    if (!IsTimeValid((time_t)d.now)) {
        DrawNoDataScreen(ICON_CLOUD);
        return;
    }
    DrawBorder(2, false);
    const double kSynodic = 29.530588853;            // лунный месяц, дней
    const double kNewMoonRef = 947182440.0;          // новолуние 6 января 2000
    double days = ((double)d.now - kNewMoonRef) / 86400.0;
    double age = fmod(days, kSynodic);
    if (age < 0) age += kSynodic;
    double phase = age / kSynodic;                   // 0 новолуние, 0.5 полнолуние
    int illum = (int)lround((1.0 - cos(2.0 * M_PI * phase)) / 2.0 * 100.0);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", illum);
    DrawTextCenter(buf, 100, 12, 2);

    // Луна: освещённая часть белая, тень чёрная. Растущая светится справа.
    const int mcx = 100, mcy = 90, r = 52;
    float k = cosf(2.0f * (float)M_PI * (float)phase);
    bool waxing = phase < 0.5;
    for (int y = -r; y <= r; y++) {
        float half = sqrtf((float)(r * r - y * y));
        float term = half * k;
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y > r * r) continue;
            float xx = waxing ? (float)x : (float)-x;
            if (xx <= term) {
                EPD_DrawColorPixel(mcx + x, mcy + y, DRIVER_COLOR_BLACK);
            }
        }
    }
    DrawRing(mcx, mcy, r, 2);
    DrawHLine(12, 187, 152, 1);

    // сколько дней до полнолуния и до новолуния
    double to_full = fmod(0.5 - phase + 1.0, 1.0) * kSynodic;
    double to_new = fmod(1.0 - phase, 1.0) * kSynodic;
    DrawRing(22, 168, 8, 2);
    snprintf(buf, sizeof(buf), "%dd", (int)lround(to_full));
    DrawPixelText(buf, 36, 162, 2);
    DrawVLine(100, 158, 184, 1);
    FillCircle(118, 168, 8);
    snprintf(buf, sizeof(buf), "%dd", (int)lround(to_new));
    DrawPixelText(buf, 132, 162, 2);
}

// ----- Батарея за неделю -----
void CustomLcdDisplay::DrawBatteryScreen(const DeskScreenData& d) {
    DrawBorder(2, false);
    char buf[16];

    // крупная батарейка и процент
    const int bx = 12, by = 10;
    DrawHLine(bx, bx + 35, by, 1);
    DrawHLine(bx, bx + 35, by + 17, 1);
    DrawVLine(bx, by, by + 17, 1);
    DrawVLine(bx + 35, by, by + 17, 1);
    FillRect(bx + 36, by + 5, bx + 39, by + 12);
    if (d.battery >= 0) {
        int fill = 30 * d.battery / 100;
        if (d.battery > 0 && fill == 0) fill = 1;
        if (fill > 0) FillRect(bx + 3, by + 3, bx + 2 + fill, by + 14);
        snprintf(buf, sizeof(buf), "%d%%", d.battery);
        DrawPixelText(buf, 58, 12, 2);
    }
    // примерно сколько дней осталось
    DrawBitmap(ICON_HOURGLASS, ICON_SIZE, ICON_SIZE, 120, 9, 1);
    if (d.batt_days_left >= 0) {
        snprintf(buf, sizeof(buf), "~%dd", d.batt_days_left);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    DrawTextRight(buf, 188, 12, 2);
    DrawHLine(12, 187, 34, 1);

    // график 0..100% за 7 дней
    const int x0 = 30, x1 = 186, y0 = 42, y1 = 158;
    DrawTextRight("100", 27, y0 - 2, 1);
    DrawTextRight("50", 27, (y0 + y1) / 2 - 3, 1);
    DrawTextRight("0", 27, y1 - 4, 1);
    DrawDottedHLine(x0, x1, y0);
    DrawDottedHLine(x0, x1, (y0 + y1) / 2);
    DrawDottedHLine(x0, x1, y1);
    const int span = 7 * 24 * 3600;
    const int64_t win_t0 = d.now - span;
    PlotSeries(d.batt_hist, DESK_BATT_POINTS, d.batt_t0, 3600, win_t0, span,
               x0, x1, y0, y1, 0.0f, 100.0f, 2, false);

    // полночь каждого дня: засечка, число месяца посередине дня
    if (IsTimeValid((time_t)d.now)) {
        time_t tt = (time_t)win_t0;
        struct tm tm_d;
        localtime_r(&tt, &tm_d);
        tm_d.tm_hour = 0;
        tm_d.tm_min = 0;
        tm_d.tm_sec = 0;
        tm_d.tm_isdst = -1;
        tm_d.tm_mday += 1;
        time_t midnight = mktime(&tm_d);
        for (int i = 0; i < 8; i++) {
            int64_t t = (int64_t)midnight + (int64_t)i * 86400;
            if (t > win_t0 + span) break;
            int x = x0 + (int)((t - win_t0) * (x1 - x0) / span);
            DrawVLine(x, y1 + 2, y1 + 5, 1);
            int xl = x0 + (int)((t + 43200 - win_t0) * (x1 - x0) / span);
            if (xl > x1 - 4) continue;
            time_t tl = (time_t)(t + 43200);
            struct tm tm_l;
            localtime_r(&tl, &tm_l);
            snprintf(buf, sizeof(buf), "%d", tm_l.tm_mday);
            DrawTextCenter(buf, xl, y1 + 10, 1);
        }
        // день, который начался до окна, подписываем, если он влезает
        int64_t t_first_mid = (int64_t)midnight;
        int xl0 = x0 + (int)((t_first_mid - 43200 - win_t0) * (x1 - x0) / span);
        if (xl0 >= x0 + 6) {
            time_t tl = (time_t)(t_first_mid - 43200);
            struct tm tm_l;
            localtime_r(&tl, &tm_l);
            snprintf(buf, sizeof(buf), "%d", tm_l.tm_mday);
            DrawTextCenter(buf, xl0, y1 + 10, 1);
        }
    }
}

// ===== Вывод на экран после пробуждения из сна =====

// После сна память контроллера экрана пустая. Кладём в неё то, что было
// на экране до сна, и обновляем только разницу: так нет мигания.
// Если старого кадра нет, делаем полное обновление с миганием.
void CustomLcdDisplay::PresentAfterWake(const uint8_t* old_frame) {
    const int len = lcd_spi_data.buffer_len;
    EPD_Init();
    if (old_frame == nullptr) {
        EPD_DisplayPartBaseImage();
        return;
    }
    EPD_SendCommand(0x24);
    writeBytes(old_frame, len);
    EPD_SendCommand(0x26);
    writeBytes(old_frame, len);
    EPD_Init_Partial();
    EPD_DisplayPart();
}

// Полоска внизу: какая страница из скольких. Пунктир на всю ширину,
// сплошная часть 2 пикселя растёт слева направо с каждой страницей.
void CustomLcdDisplay::DrawPageBar(int page, int count) {
    const int x0 = 4, x1 = Width - 5, y = Height - 6;
    for (int x = x0; x <= x1; x += 3) {
        EPD_DrawColorPixel(x, y + 1, DRIVER_COLOR_BLACK);
    }
    int end = x0 + (page + 1) * (x1 - x0) / count;
    DrawHLine(x0, end, y, 2);
}

void CustomLcdDisplay::RenderDeskPage(int page, const DeskScreenData& d, const uint8_t* old_frame,
                                      bool page_bar) {
    std::lock_guard<std::recursive_mutex> lock(draw_mutex_);
    EPD_Clear();
    switch (page) {
        case DESK_PAGE_WEATHER:
            DrawWeatherScreen(d.weather);
            break;
        case DESK_PAGE_ROOM:
            if (d.room_ok) {
                DrawRoomScreen(d.room_t, d.room_h);
            } else {
                DrawNoDataScreen(ICON_HOUSE);   // датчик не ответил
            }
            break;
        case DESK_PAGE_OUT24:
            DrawOut24Screen(d);
            break;
        case DESK_PAGE_ROOM24:
            DrawRoom24Screen(d);
            break;
        case DESK_PAGE_BOTH:
            DrawBothScreen(d);
            break;
        case DESK_PAGE_SUN:
            DrawSunScreen(d);
            break;
        case DESK_PAGE_MOON:
            DrawMoonScreen(d);
            break;
        case DESK_PAGE_BATTERY:
            DrawBatteryScreen(d);
            break;
        default:
            DrawClockScreen(d);
            break;
    }
    if (page_bar) {
        DrawPageBar(page, DESK_PAGE_COUNT);
    }
    PresentAfterWake(old_frame);
}

void CustomLcdDisplay::RenderFaceAfterWake(int face_type, const uint8_t* old_frame) {
    PaintFace(face_type);
    PresentAfterWake(old_frame);
}

void CustomLcdDisplay::EPD_Sleep() {
    EPD_SendCommand(0x10);   // deep sleep контроллера
    EPD_SendData(0x01);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// ===== Виджеты по голосу =====

void CustomLcdDisplay::ShowWeatherWidget(const WeatherCache& w, int seconds) {
    if (face_mode_ == FaceMode::kShuttingDown) return;
    if (booting_) return;

    // Если предыдущий виджет висит совсем недолго, встаём в очередь.
    int64_t started_check = esp_timer_get_time() / 1000;
    if (face_mode_ == FaceMode::kWidget && widget_timer_ != nullptr) {
        int64_t shown = started_check - widget_started_ms_;
        if (shown < kMinWidgetMs) {
            pending_widget_ = true;
            pending_kind_ = 1;
            pending_weather_ = w;
            pending_seconds_ = seconds;
            esp_timer_stop(widget_timer_);
            esp_timer_start_once(widget_timer_, (int64_t)(kMinWidgetMs - shown) * 1000);
            return;
        }
    }

    std::lock_guard<std::recursive_mutex> lock(draw_mutex_);
    face_mode_ = FaceMode::kWidget;
    current_face_ = -1;
    EPD_Clear();
    DrawWeatherScreen(w);
    EPD_DisplayPart();
    StartWidgetTimer(seconds);
}

void CustomLcdDisplay::ShowRoomWidget(float temperature, float humidity, int seconds) {
    if (face_mode_ == FaceMode::kShuttingDown) return;
    if (booting_) return;

    // Если предыдущий виджет висит совсем недолго, встаём в очередь.
    int64_t started_check = esp_timer_get_time() / 1000;
    if (face_mode_ == FaceMode::kWidget && widget_timer_ != nullptr) {
        int64_t shown = started_check - widget_started_ms_;
        if (shown < kMinWidgetMs) {
            pending_widget_ = true;
            pending_kind_ = 2;
            pending_temp_ = temperature;
            pending_hum_ = humidity;
            pending_seconds_ = seconds;
            esp_timer_stop(widget_timer_);
            esp_timer_start_once(widget_timer_, (int64_t)(kMinWidgetMs - shown) * 1000);
            return;
        }
    }

    std::lock_guard<std::recursive_mutex> lock(draw_mutex_);
    face_mode_ = FaceMode::kWidget;
    current_face_ = -1;
    EPD_Clear();
    DrawRoomScreen(temperature, humidity);
    EPD_DisplayPart();
    StartWidgetTimer(seconds);
}

// Таймер, по которому виджет уходит (или показывается следующий из очереди).
void CustomLcdDisplay::StartWidgetTimer(int seconds) {
    if (widget_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<CustomLcdDisplay*>(arg)->OnWidgetTimer();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "widget_hide";
        esp_timer_create(&args, &widget_timer_);
    }
    esp_timer_stop(widget_timer_);
    widget_started_ms_ = esp_timer_get_time() / 1000;
    esp_timer_start_once(widget_timer_, (int64_t)seconds * 1000000);
}

void CustomLcdDisplay::HideWidget() {
    if (face_mode_ != FaceMode::kWidget) return;
    face_mode_ = FaceMode::kIdle;

    auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        SetStatus(Lang::Strings::SPEAKING);
    } else if (state == kDeviceStateListening) {
        SetStatus(Lang::Strings::LISTENING);
    }
    force_redraw_ = true;
}

void CustomLcdDisplay::OnWidgetTimer() {
    // Если за это время устройство начало выключаться, очередь выбрасываем,
    // чтобы виджет не нарисовался поверх грустного лица.
    if (face_mode_ != FaceMode::kWidget) {
        pending_widget_ = false;
        return;
    }
    if (pending_widget_) {
        pending_widget_ = false;
        int kind = pending_kind_;
        int secs = pending_seconds_;
        face_mode_ = FaceMode::kIdle;   // чтобы показ не отложился снова
        if (kind == 1) {
            ShowWeatherWidget(pending_weather_, secs);
        } else {
            ShowRoomWidget(pending_temp_, pending_hum_, secs);
        }
        return;
    }
    HideWidget();
}

// =====================================================================
// ============================ ЖИВОЕ ЛИЦО ==============================
// =====================================================================

// Эмоции, которые присылает сервер, и как они выглядят: глаза и рот
// "в покое". Во время речи рот двигается, а глаза остаются от эмоции.
struct EmotionStyle {
    const char* name;
    int8_t eyes;
    int8_t mouth;
};

static const EmotionStyle kEmotionStyles[] = {
    { "neutral",     EYES_OPEN,     MOUTH_IDLE },   // [0] обычное лицо
    { "happy",       EYES_HAPPY,    MOUTH_SMILE },
    { "laughing",    EYES_HAPPY,    MOUTH_GRIN },
    { "funny",       EYES_HAPPY,    MOUTH_GRIN },
    { "relaxed",     EYES_HAPPY,    MOUTH_IDLE },
    { "confident",   EYES_OPEN,     MOUTH_SMILE },
    { "delicious",   EYES_HAPPY,    MOUTH_TONGUE },
    { "loving",      EYES_LOVE,     MOUTH_SMILE },
    { "kissy",       EYES_WINK_R,   MOUTH_O },
    { "winking",     EYES_WINK_R,   MOUTH_SMILE },
    { "silly",       EYES_WINK_L,   MOUTH_TONGUE },
    { "cool",        EYES_COOL,     MOUTH_IDLE },
    { "sad",         EYES_SAD,      MOUTH_SAD },
    { "crying",      EYES_CRY,      MOUTH_SAD },
    { "angry",       EYES_ANGRY,    MOUTH_SAD },
    { "surprised",   EYES_BIG,      MOUTH_O },
    { "shocked",     EYES_BIG,      MOUTH_O },
    { "thinking",    EYES_UP_L,     MOUTH_THINK },
    { "confused",    EYES_CONFUSED, MOUTH_WAVY },
    { "embarrassed", EYES_LOOK_DOWN, MOUTH_WAVY },
    { "sleepy",      EYES_SLEEPY,   MOUTH_FLAT },
};
static const int kEmotionCount = sizeof(kEmotionStyles) / sizeof(kEmotionStyles[0]);

// Настройки живости. Меняй смело.
static constexpr int kAnimTickMs = 250;              // как часто аниматор думает
static constexpr int kIdleSleepyMs = 3 * 60 * 1000;  // через сколько простоя становится сонным
static constexpr int kIdleAsleepMs = 6 * 60 * 1000;  // через сколько засыпает (Zzz)
static constexpr int kSleepZzzMs = 3000;             // смена кадров Zzz
static constexpr int kSleepAnimMs = 20 * 60 * 1000;  // сколько Zzz анимируется, потом замирает (бережём экран)
static constexpr int kThinkAfterSilenceMs = 800;     // тишина после твоей фразы, и он "задумался"
static constexpr int kThinkMaxMs = 12000;            // дольше не думает, возвращается к слушанию

static int RandRange(int lo, int hi) {
    return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1));
}

void CustomLcdDisplay::StartAnimator() {
    if (anim_timer_ != nullptr) return;
    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        static_cast<CustomLcdDisplay*>(arg)->AnimTick();
    };
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "face_anim";
    args.skip_unhandled_events = true;
    if (esp_timer_create(&args, &anim_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Face animator: failed to create timer");
        return;
    }
    last_tick_ms_ = esp_timer_get_time() / 1000;
    esp_timer_start_periodic(anim_timer_, (int64_t)kAnimTickMs * 1000);
}

void CustomLcdDisplay::StartSequence(const AnimFrame* frames, int count) {
    if (count > (int)(sizeof(seq_) / sizeof(seq_[0]))) {
        count = sizeof(seq_) / sizeof(seq_[0]);
    }
    for (int i = 0; i < count; i++) seq_[i] = frames[i];
    seq_len_ = count;
    seq_pos_ = 0;
    seq_left_ms_ = count > 0 ? frames[0].ms : 0;
}

static CustomLcdDisplay::AnimFrame MakeFrame(int eyes, int mouth, int ms) {
    CustomLcdDisplay::AnimFrame f;
    f.full = -1;
    f.eyes = (int8_t)eyes;
    f.mouth = (int8_t)mouth;
    f.ms = (uint16_t)ms;
    return f;
}

static CustomLcdDisplay::AnimFrame MakeFull(int face, int ms) {
    CustomLcdDisplay::AnimFrame f;
    f.full = (int16_t)face;
    f.eyes = -1;
    f.mouth = -1;
    f.ms = (uint16_t)ms;
    return f;
}

// Случайная выходка в простое: моргнуть, посмотреть, зевнуть...
void CustomLcdDisplay::PickIdleEvent() {
    const int M = MOUTH_IDLE;
    int r = RandRange(0, 99);

    if (mode_ms_ >= kIdleSleepyMs) {
        // сонный: медленно моргает и зевает
        if (r < 60) {
            AnimFrame f[] = { MakeFrame(EYES_CLOSED, MOUTH_FLAT, 900) };
            StartSequence(f, 1);
        } else {
            AnimFrame f[] = { MakeFrame(EYES_CLOSED, MOUTH_YAWN, 1500),
                              MakeFrame(EYES_SLEEPY, MOUTH_FLAT, 700) };
            StartSequence(f, 2);
        }
        next_event_ms_ = RandRange(6000, 12000);
        return;
    }

    if (r < 28) {            // моргнуть
        AnimFrame f[] = { MakeFrame(EYES_CLOSED, M, 300) };
        StartSequence(f, 1);
    } else if (r < 38) {     // моргнуть дважды
        AnimFrame f[] = { MakeFrame(EYES_CLOSED, M, 300), MakeFrame(EYES_OPEN, M, 300),
                          MakeFrame(EYES_CLOSED, M, 300) };
        StartSequence(f, 3);
    } else if (r < 50) {     // посмотреть в сторону
        AnimFrame f[] = { MakeFrame(RandRange(0, 1) ? EYES_LOOK_L : EYES_LOOK_R, M, 1500) };
        StartSequence(f, 1);
    } else if (r < 57) {     // вверх или вниз
        AnimFrame f[] = { MakeFrame(RandRange(0, 1) ? EYES_LOOK_UP : EYES_LOOK_DOWN, M, 1200) };
        StartSequence(f, 1);
    } else if (r < 65) {     // оглядеться
        AnimFrame f[] = { MakeFrame(EYES_LOOK_L, M, 900), MakeFrame(EYES_LOOK_R, M, 900),
                          MakeFrame(EYES_OPEN, M, 400), MakeFrame(EYES_CLOSED, M, 300) };
        StartSequence(f, 4);
    } else if (r < 71) {     // подмигнуть
        AnimFrame f[] = { MakeFrame(RandRange(0, 1) ? EYES_WINK_L : EYES_WINK_R, MOUTH_SMILE, 900) };
        StartSequence(f, 1);
    } else if (r < 79) {     // улыбнуться
        AnimFrame f[] = { MakeFrame(EYES_HAPPY, MOUTH_SMILE, 1600) };
        StartSequence(f, 1);
    } else if (r < 85) {     // заскучать
        AnimFrame f[] = { MakeFrame(EYES_CLOSED, MOUTH_BORED, 2000) };
        StartSequence(f, 1);
    } else if (r < 90) {     // о чём-то задумался
        AnimFrame f[] = { MakeFrame(EYES_UP_L, MOUTH_THINK, 1200), MakeFrame(EYES_UP_R, MOUTH_THINK, 1200) };
        StartSequence(f, 2);
    } else if (r < 94) {     // подразнить
        AnimFrame f[] = { MakeFrame(EYES_WINK_L, MOUTH_TONGUE, 1200) };
        StartSequence(f, 1);
    } else if (mode_ms_ > 60000) {   // зевнуть, если давно никого нет
        AnimFrame f[] = { MakeFrame(EYES_CLOSED, MOUTH_YAWN, 1500), MakeFrame(EYES_SLEEPY, MOUTH_FLAT, 600) };
        StartSequence(f, 2);
    } else {
        AnimFrame f[] = { MakeFrame(EYES_CLOSED, M, 300) };
        StartSequence(f, 1);
    }
    next_event_ms_ = RandRange(3000, 9000);
}

// Пока слушает: моргает и иногда косится в сторону.
void CustomLcdDisplay::PickListenEvent() {
    int r = RandRange(0, 99);
    if (r < 75) {
        AnimFrame f[] = { MakeFrame(EYES_CLOSED, MOUTH_DOT, 300) };
        StartSequence(f, 1);
    } else {
        AnimFrame f[] = { MakeFrame(RandRange(0, 1) ? EYES_LOOK_L : EYES_LOOK_R, MOUTH_DOT, 1000) };
        StartSequence(f, 1);
    }
    next_event_ms_ = RandRange(3000, 6000);
}

// Главное решение: какое лицо сейчас показать. dt - сколько прошло мс.
CustomLcdDisplay::AnimFrame CustomLcdDisplay::DecideFrame(int dt) {
    FaceMode mode = face_mode_;

    // Сменился режим: сбрасываем сценку и счётчики.
    if (mode != last_mode_) {
        last_mode_ = mode;
        mode_ms_ = 0;
        seq_len_ = 0;
        force_redraw_ = true;
        heard_voice_ = false;
        silence_ms_ = 0;
        think_ms_ = 0;
        play_ms_ = 0;
        speak_mouth_ = MOUTH_FLAT;
        speak_mouth_left_ms_ = 0;
        next_event_ms_ = (mode == FaceMode::kIdle) ? RandRange(2500, 5000) : RandRange(2500, 4500);
    } else {
        mode_ms_ += dt;
    }

    int emo = emotion_;
    if (emo < 0 || emo >= kEmotionCount) emo = 0;
    const EmotionStyle& style = kEmotionStyles[emo];

    // Идёт сценка: доигрываем её.
    if (seq_len_ > 0) {
        seq_left_ms_ -= dt;
        while (seq_len_ > 0 && seq_left_ms_ <= 0) {
            seq_pos_++;
            if (seq_pos_ >= seq_len_) {
                seq_len_ = 0;
            } else {
                seq_left_ms_ += seq_[seq_pos_].ms;
            }
        }
        if (seq_len_ > 0) {
            return seq_[seq_pos_];
        }
    }

    switch (mode) {
        case FaceMode::kSpeaking: {
            // Рот двигается, только когда звук реально играет, и каждый
            // раз немного по-разному. Глаза от эмоции, иногда моргает.
            bool playing = !Application::GetInstance().GetAudioService().IsPlaybackIdle();
            play_ms_ = playing ? play_ms_ + dt : 0;
            int mouth = (style.mouth == MOUTH_IDLE) ? (int)MOUTH_FLAT : (int)style.mouth;
            if (play_ms_ >= kAnimTickMs) {
                speak_mouth_left_ms_ -= dt;
                if (speak_mouth_left_ms_ <= 0) {
                    static const int8_t kOpen[] = { MOUTH_OPEN, MOUTH_OPEN_S, MOUTH_OPEN_W, MOUTH_OPEN,
                                                    MOUTH_FLAT };
                    int next = speak_mouth_;
                    for (int tries = 0; tries < 4 && next == speak_mouth_; tries++) {
                        next = kOpen[RandRange(0, sizeof(kOpen) - 1)];
                    }
                    speak_mouth_ = next;
                    speak_mouth_left_ms_ = RandRange(400, 700);
                }
                mouth = speak_mouth_;
            } else {
                speak_mouth_ = mouth;
            }
            int eyes = style.eyes;
            speak_blink_ms_ -= dt;
            if (speak_blink_ms_ <= 0) {
                speak_blink_ms_ = RandRange(3500, 7000);
                if (eyes == EYES_OPEN || eyes == EYES_HAPPY || eyes == EYES_SAD) {
                    return MakeFrame(EYES_CLOSED, mouth, 300);
                }
            }
            return MakeFrame(eyes, mouth, 0);
        }

        case FaceMode::kEmotion: {
            // Эмоция из ответа, пару секунд после речи.
            emotion_hold_ms_ -= dt;
            if (emotion_hold_ms_ <= 0) {
                emotion_ = 0;
                face_mode_ = FaceMode::kListening;
            }
            return MakeFrame(style.eyes, style.mouth, 0);
        }

        case FaceMode::kListening: {
            bool voice = Application::GetInstance().GetAudioService().IsVoiceDetected();
            if (voice) {
                // слышит твой голос: глаза широко открыты
                heard_voice_ = true;
                silence_ms_ = 0;
                think_ms_ = 0;
                seq_len_ = 0;
                return MakeFrame(EYES_OPEN, MOUTH_DOT, 0);
            }
            if (heard_voice_) {
                silence_ms_ += dt;
                if (silence_ms_ >= kThinkAfterSilenceMs) {
                    // ты договорил: он задумался, пока ждёт ответ
                    think_ms_ += dt;
                    if (think_ms_ > kThinkMaxMs) {
                        heard_voice_ = false;
                        think_ms_ = 0;
                    } else {
                        bool left = ((think_ms_ / 900) % 2) == 0;
                        return MakeFrame(left ? EYES_UP_L : EYES_UP_R, MOUTH_THINK, 0);
                    }
                } else {
                    return MakeFrame(EYES_OPEN, MOUTH_DOT, 0);
                }
            }
            next_event_ms_ -= dt;
            if (next_event_ms_ <= 0) {
                PickListenEvent();
                if (seq_len_ > 0) return seq_[0];
            }
            return MakeFrame(EYES_HALF, MOUTH_DOT, 0);
        }

        case FaceMode::kIdle: {
            // Слабый Wi-Fi: мигающий значок.
            if (wifi_weak_request_) {
                wifi_weak_request_ = false;
                AnimFrame f[] = { MakeFull(FACE_WIFI_WEAK, 800), MakeFull(FACE_WIFI_WEAK2, 800),
                                  MakeFull(FACE_WIFI_WEAK, 800), MakeFull(FACE_WIFI_WEAK2, 800),
                                  MakeFull(FACE_WIFI_WEAK, 800) };
                StartSequence(f, 5);
                return seq_[0];
            }
            // Давно никого: спит и видит сны.
            if (mode_ms_ >= kIdleAsleepMs) {
                bool z1 = ((mode_ms_ / kSleepZzzMs) % 2) == 0;
                if (mode_ms_ >= kIdleAsleepMs + kSleepAnimMs) {
                    z1 = false;   // крепко спит: картинка больше не меняется
                    mode_ms_ = kIdleAsleepMs + kSleepAnimMs;   // счётчик дальше не растёт
                }
                return MakeFrame(z1 ? EYES_SLEEP_Z1 : EYES_SLEEP_Z2, MOUTH_FLAT, 0);
            }
            next_event_ms_ -= dt;
            if (next_event_ms_ <= 0) {
                PickIdleEvent();
                if (seq_len_ > 0) return seq_[0];
            }
            if (mode_ms_ >= kIdleSleepyMs) {
                return MakeFrame(EYES_SLEEPY, MOUTH_FLAT, 0);
            }
            return MakeFrame(EYES_OPEN, MOUTH_IDLE, 0);
        }

        default:
            return MakeFull(-1, 0);
    }
}

// Рисует составное лицо в буфер: глаза сверху, рот снизу.
void CustomLcdDisplay::PaintFaceParts(int eyes, int mouth) {
    const int cell = Width / FACE_GRID;
    const int offset = (Width - cell * FACE_GRID) / 2;
    EPD_Clear();
    for (int row = 0; row < FACE_GRID; row++) {
        const char* line = nullptr;
        if (row < FACE_HALF) {
            if (eyes >= 0 && eyes < EYES_COUNT) line = kEyes[eyes][row];
        } else {
            if (mouth >= 0 && mouth < MOUTH_COUNT) line = kMouths[mouth][row - FACE_HALF];
        }
        if (line == nullptr) continue;
        for (int col = 0; col < FACE_GRID; col++) {
            if (line[col] != '#') continue;
            for (int dy = 0; dy < cell; dy++) {
                for (int dx = 0; dx < cell; dx++) {
                    EPD_DrawColorPixel(offset + col * cell + dx, offset + row * cell + dy,
                                       DRIVER_COLOR_BLACK);
                }
            }
        }
    }
}

void CustomLcdDisplay::DrawAnimFrame(const AnimFrame& f) {
    std::lock_guard<std::recursive_mutex> lock(draw_mutex_);
    // Пока ждали блокировку, мог появиться виджет или начаться выключение.
    FaceMode mode = face_mode_;
    if (mode == FaceMode::kShuttingDown || mode == FaceMode::kBooting ||
        mode == FaceMode::kWidget || booting_) {
        return;
    }
    if (f.full >= 0) {
        current_face_ = f.full;
        PaintFace(f.full);
    } else {
        current_face_ = -1;
        PaintFaceParts(f.eyes, f.mouth);
    }
    EPD_DisplayPart();
    drawn_full_ = f.full;
    drawn_eyes_ = f.eyes;
    drawn_mouth_ = f.mouth;
}

void CustomLcdDisplay::AnimTick() {
    int64_t now = esp_timer_get_time() / 1000;
    int dt = (int)(now - last_tick_ms_);
    last_tick_ms_ = now;
    if (dt < 0 || dt > 5000) dt = kAnimTickMs;

    FaceMode mode = face_mode_;
    if (mode == FaceMode::kShuttingDown || mode == FaceMode::kBooting ||
        mode == FaceMode::kWidget || booting_) {
        last_mode_ = mode;
        force_redraw_ = true;   // после виджета или загрузки лицо нарисуется заново
        return;
    }

    AnimFrame f = DecideFrame(dt);
    if (f.full < 0 && f.eyes < 0) return;

    bool same = (f.full == drawn_full_) && (f.eyes == drawn_eyes_) && (f.mouth == drawn_mouth_);
    if (same && !force_redraw_) return;
    force_redraw_ = false;
    DrawAnimFrame(f);
}

void CustomLcdDisplay::SetStatus(const char* status) {
    ESP_LOGI(TAG, "SetStatus: %s", status);
    if (face_mode_ == FaceMode::kShuttingDown) return;
    if (booting_) return;
    if (face_mode_ == FaceMode::kWidget) return;   // виджет досматриваем до конца

    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        if (face_mode_ == FaceMode::kSpeaking && emotion_ != 0) {
            // Договорил с эмоцией: пару секунд держим её лицо.
            emotion_hold_ms_ = kEmotionRevertMs;
            face_mode_ = FaceMode::kEmotion;
        } else if (face_mode_ != FaceMode::kEmotion) {
            emotion_ = 0;
            face_mode_ = FaceMode::kListening;
        }
    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        face_mode_ = FaceMode::kSpeaking;
    } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        emotion_ = 0;
        face_mode_ = FaceMode::kIdle;
    }
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    if (face_mode_ == FaceMode::kShuttingDown) return;
    if (booting_) return;
    for (int i = 0; i < kEmotionCount; i++) {
        if (strcmp(emotion, kEmotionStyles[i].name) == 0) {
            emotion_ = i;
            force_redraw_ = true;
            return;
        }
    }
    // незнакомая эмоция: лицо не трогаем
}

void CustomLcdDisplay::SetupUI() {
    ESP_LOGI(TAG, "Custom SetupUI: skipping default widgets");
    Display::SetupUI();
    StartAnimator();   // лицо дальше рисует аниматор
}

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    ESP_LOGI(TAG, "ChatMessage [%s]: %s", role, content);
    // ничего не рисуем, лицо остаётся
}
void CustomLcdDisplay::SetTheme(Theme* theme) {
    ESP_LOGI(TAG, "SetTheme: skipped (custom face UI)");
    // не применяем тему, виджетов нет
}