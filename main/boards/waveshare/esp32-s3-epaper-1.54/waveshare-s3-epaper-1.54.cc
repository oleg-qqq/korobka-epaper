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

#define TAG "waveshare_epaper_1_54"

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
            display_->DrawFace(FACE_DEAD);
            vTaskDelay(pdMS_TO_TICKS(2000));
            power_->PowerAudioOff();
            power_->PowerEpdOff();
            power_->VbatPowerOff();
        });
    }

        void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();


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
                bool heads = (esp_random() & 1) != 0;
                return std::string(heads ? "heads" : "tails");
            });
		//температура и влажность на датчике
        mcp_server.AddTool("self.sensor.get_room_climate",
            "Get current room temperature in Celsius and humidity in percent from the built-in sensor",
            PropertyList(), [this](const PropertyList &) -> ReturnValue {
                float t = 0, h = 0;
                if (!ReadSHTC3(t, h)) {
                    return std::string("{\"success\":false}");
                }
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "{\"success\":true,\"temperature\":%.1f,\"humidity\":%.1f}", t, h);
                return std::string(buf);
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
			
	
    }

    void InitializeLcdDisplay() {
        custom_lcd_spi_t lcd_spi_data = {};
        lcd_spi_data.cs               = EPD_CS_PIN;
        lcd_spi_data.dc               = EPD_DC_PIN;
        lcd_spi_data.rst              = EPD_RST_PIN;
        lcd_spi_data.busy             = EPD_BUSY_PIN;
        lcd_spi_data.mosi             = EPD_MOSI_PIN;
        lcd_spi_data.scl              = EPD_SCK_PIN;
        lcd_spi_data.spi_host         = EPD_SPI_NUM;
        lcd_spi_data.buffer_len       = 5000;
        display_                      = new CustomLcdDisplay(NULL, NULL, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, lcd_spi_data);
    }

    void Power_Init() {
        power_ = new BoardPowerBsp(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
        power_->VbatPowerOn();
        power_->PowerAudioOn();
        power_->PowerEpdOn();
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

        // Wakeup
        uint8_t wakeup[2] = {0x35, 0x17};
        i2c_master_transmit(shtc3_dev, wakeup, 2, 100);
        vTaskDelay(pdMS_TO_TICKS(2));

        // Measure T first, normal mode, clock stretching disabled
        uint8_t cmd[2] = {0x78, 0x66};
        if (i2c_master_transmit(shtc3_dev, cmd, 2, 100) != ESP_OK) {
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

        // Sleep
        uint8_t sleep_cmd[2] = {0xB0, 0x98};
        i2c_master_transmit(shtc3_dev, sleep_cmd, 2, 100);

        uint16_t raw_t = (data[0] << 8) | data[1];
        uint16_t raw_h = (data[3] << 8) | data[4];

        temperature = -45.0f + 175.0f * (float)raw_t / 65535.0f - 4.7f;
        humidity    = 100.0f * (float)raw_h / 65535.0f + 3.0f;

        ESP_LOGI(TAG, "SHTC3: %.1f C, %.1f %%", temperature, humidity);
        return true;
    }
	
  public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), pwr_button_(VBAT_PWR_GPIO) {
        Power_Init();
        InitializeI2c();
        InitializeButtons();
        InitializeTools();
        InitializeLcdDisplay();
    }

    virtual AudioCodec *GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
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