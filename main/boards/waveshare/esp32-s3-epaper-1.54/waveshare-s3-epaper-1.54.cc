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

#define TAG "waveshare_epaper_1_54"

// ===== ПОГОДА (Open-Meteo, Лимассол, Кипр) =====
// Вставить после блока с монеткой (self.game.coin_flip)
 
// Координаты Лимассола, Кипр
static constexpr double kWeatherLat = 34.7071;
static constexpr double kWeatherLon = 33.0226;
 
// Простейший кэш, чтобы не долбить API при каждом вопросе / каждом
// обновлении заставки. Живёт 10 минут.
struct WeatherCache {
    std::string summary;      // готовая фраза для голоса
    float temperature = 0.0f; // для заставки на экране
    int weather_code = 0;     // код погоды Open-Meteo (для иконки)
    int64_t fetched_at_ms = 0;
    bool valid = false;
};
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
 
// Делает запрос к Open-Meteo и обновляет кэш.
// Возвращает true при успехе.
static bool FetchWeather() {
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code&timezone=auto",
             kWeatherLat, kWeatherLon);
 
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
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
    std::string desc = WeatherCodeToText(weather_code);
 
    char summary[128];
    snprintf(summary, sizeof(summary),
             "В Лимассоле сейчас %s, температура %.0f градусов",
             desc.c_str(), temperature);
 
    s_weather_cache.summary = summary;
    s_weather_cache.temperature = temperature;
    s_weather_cache.weather_code = weather_code;
    s_weather_cache.fetched_at_ms = esp_timer_get_time() / 1000;
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
        if (!FetchWeather() && !s_weather_cache.valid) {
            return false; // не было ни разу удачного запроса
        }
    }
    out = s_weather_cache;
    return true;
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
	int idle_face_tick_ = 0;
    int idle_blink_in_ = 30;     // через сколько тиков следующее моргание
    int idle_bored_in_ = 300;    // через сколько тиков следующая "скука"
    bool idle_bored_active_ = false;
	esp_timer_handle_t boot_timer_ = nullptr;
    int boot_frame_ = 0;
	
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
 
  void UpdateIdleFace() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle ||
            display_->GetFaceMode() != FaceMode::kIdle) {
            idle_face_tick_ = 0;
            idle_bored_active_ = false;
            return;
        }

        idle_face_tick_++;

        // скучающее лицо держим примерно полторы секунды, потом обратно
        if (idle_bored_active_) {
            if (idle_face_tick_ % 4 == 0) {
                idle_bored_active_ = false;
                display_->DrawFace(FACE_IDLE);
                idle_bored_in_ = idle_face_tick_ + 60 + (esp_random() % 90);
            }
            return;
        }

        // моргание, короткая вспышка на один тик
        if (idle_face_tick_ == idle_blink_in_) {
            display_->DrawFace(FACE_IDLE_BLINK);
        } else if (idle_face_tick_ == idle_blink_in_ + 1) {
            display_->DrawFace(FACE_IDLE);
            idle_blink_in_ = idle_face_tick_ + 10 + (esp_random() % 15);
        }

        // скучающее лицо, редкое событие
        if (idle_face_tick_ == idle_bored_in_) {
            idle_bored_active_ = true;
            display_->DrawFace(FACE_IDLE_BORED);
        }
    }
 
 
    // Запускает таймер мигания. Период 400 мс даёт спокойное,
    // заметное мигание. Меньше значение, быстрее моргает.
    void StartListenLed() {
        esp_timer_create_args_t args = {};
       args.callback = [](void* arg) {
    auto* self = static_cast<CustomBoard*>(arg);
    self->UpdateListenLed();
    self->UpdateIdleFace();  
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
    "Announce the result to the user in their language.",
    PropertyList(),
    [this](const PropertyList &) -> ReturnValue {
        WeatherCache w;
        if (!GetWeather(w)) {
            return std::string("Не удалось получить данные о погоде, попробуйте позже");
        }
        return w.summary;
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

// Проверяет, готово ли устройство, и если да, запускает прослушку.
    // Вызывается таймером раз в секунду.
    void TryAutoListen() {
        auto& app = Application::GetInstance();
        autolisten_retries_++;
 
        // kDeviceStateIdle означает: загрузка закончена, Wi-Fi поднят,
        // протокол готов, устройство просто ждёт команды.
        if (app.GetDeviceState() == kDeviceStateIdle) {
            ESP_LOGI(TAG, "Auto listen: device ready, starting chat");
            esp_timer_stop(autolisten_timer_);
            app.ToggleChatState();
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
 static constexpr int kBootFrameMs = 2000;   // 6 кадров, всего 12 секунд

 void ShowBootProgress() {
    display_->SetBooting(true);
    boot_frame_ = 0;
    display_->DrawFace(FACE_BOOT_0);

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
    esp_timer_start_periodic(boot_timer_, kBootFrameMs * 1000);
}

 void NextBootFrame() {
    static const int frames[] = {
        FACE_BOOT_0, FACE_BOOT_1, FACE_BOOT_2,
        FACE_BOOT_3, FACE_BOOT_4, FACE_BOOT_5
    };
    const int count = sizeof(frames) / sizeof(frames[0]);

    boot_frame_++;
    if (boot_frame_ >= count) {
        esp_timer_stop(boot_timer_);
        display_->SetBooting(false);
        display_->RequestFace();
        return;
    }
    display_->DrawFace(frames[boot_frame_]);
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
        ShowBootProgress();
        StartAutoListen();
        StartListenLed();
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

