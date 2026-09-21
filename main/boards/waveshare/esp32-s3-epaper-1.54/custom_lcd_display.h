#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include "lcd_display.h"
#include "pixel_face.h"
/* Display color */
typedef enum {
    DRIVER_COLOR_WHITE  = 0xff,
    DRIVER_COLOR_BLACK  = 0x00,
    FONT_BACKGROUND = DRIVER_COLOR_WHITE,
}COLOR_IMAGE;

enum class FaceMode {
    kShuttingDown,
    kBooting,
    kSpeaking,
    kListening,
    kEmotion,
    kIdle,
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
                  bool mirror_x, bool mirror_y, bool swap_xy,custom_lcd_spi_t _lcd_spi_data);
    ~CustomLcdDisplay();

    void EPD_Init();    /* e-paper init */
	void EPD_Clear();   /* clear screen */
	void EPD_Display(); /* flush buffer to e-paper */

	/* fast refresh */
	void EPD_DisplayPartBaseImage();
    void EPD_Init_Partial();
    void EPD_DisplayPart();
    void EPD_DrawColorPixel(uint16_t x, uint16_t y,uint8_t color);
         
		 /* Pixel art face */
    void DrawFace(int face_type);
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
	virtual void SetupUI() override;
	virtual void SetChatMessage(const char* role, const char* content) override;
	virtual void SetTheme(Theme* theme) override;
	void SetShuttingDown(bool value) {
    face_mode_ = value ? FaceMode::kShuttingDown : FaceMode::kIdle;
    if (value) {
        if (mouth_timer_) esp_timer_stop(mouth_timer_);
        if (emotion_revert_timer_) esp_timer_stop(emotion_revert_timer_);
    }
}

void SetBooting(bool value) {
    booting_ = value;
    if (face_mode_ == FaceMode::kShuttingDown) return;
    face_mode_ = value ? FaceMode::kBooting : FaceMode::kIdle;
}

FaceMode GetFaceMode() const { return face_mode_; }

void RequestFace();

	
private:
    int current_face_ = -1;
	 int partial_count_ = 0;
    const custom_lcd_spi_t lcd_spi_data;
    const int Width;
    const int Height;
    spi_device_handle_t spi;
    uint8_t *buffer = NULL;
    static void lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);
    void spi_gpio_init();
    void spi_port_init();
    void read_busy();
FaceMode face_mode_ = FaceMode::kIdle;
    bool booting_ = false;
     bool mouth_open_ = true;
    esp_timer_handle_t mouth_timer_ = nullptr;
     esp_timer_handle_t emotion_revert_timer_ = nullptr;
     int emotion_face_ = FACE_HAPPY;   // какую именно эмоцию сейчас показываем
	 int pending_emotion_ = -1;   // эмоция, отложенная до конца речи

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
	void RevertToSpeakingFace();
	void ToggleMouth();
};

#endif // __CUSTOM_LCD_DISPLAY_H__