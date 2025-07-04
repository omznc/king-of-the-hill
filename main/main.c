#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_ops.h"
#include "ArcadePix9x11.c"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "freertos/queue.h"

// Konstante
#define LEFT_BUTTON_PIN 47
#define RIGHT_BUTTON_PIN 21
#define BUZZER_PIN 46
#define LED_STRIP_PIN 3
#define DISPLAY_SDA_PIN 48 // Pin za podatke displeja
#define DISPLAY_SCL_PIN 45 // Pin za sat displeja

#define FONT_HEIGHT 11
#define FONT_SPACING 1
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz rezolucija
#define RMT_LED_STRIP_GPIO_NUM 3
#define NUMBER_OF_LEDS 40

#define I2C_HOST 0
#define DISPLAY_I2C_ADDR 0x3C

#define BUZZER_EVENT_BIT (1 << 0)

#define WIFI_SSID "kingofthehill"
#define WIFI_PASS "12345678"
#define NTFY_ENDPOINT "http://ntfy.sh/king-of-the-hill-omznc"

#define DEVICE_NAME "omznc-koth"

// Red za slanje poruka mrežnom zadatku
#define QUEUE_SIZE 10
static QueueHandle_t network_queue;

// Enumeracije
typedef enum
{
    GAME_OFF,
    GAME_PLAYING,
    GAME_FINISHED
} GameState;

typedef enum
{
    NONE,
    LEFT_RED,
    RIGHT_BLUE,
} TeamColor;

typedef enum
{
    BUZZER_OFF,
    BUZZER_SECONDS,
    BUZZER_FINISHED
} BuzzerState;

// Globalne varijable
static BuzzerState buzzer_state = BUZZER_OFF;
static GameState game_state = GAME_OFF;
static TeamColor team_color = NONE;
static const int game_time_seconds = 900; // 15 minuta
static int current_game_time = 0;
static esp_lcd_panel_handle_t display_panel = NULL;
static uint8_t led_strip_pixels[NUMBER_OF_LEDS * 4];
static bool end_game_beep_done = false;
static const char *TAG = "king-of-the-hill";

// Eksterna referenca na podatke fonta
extern const unsigned short Arcadepix9x11[];

// Strukture
typedef struct LedParams
{
    rmt_encoder_handle_t *led_encoder;
    rmt_channel_handle_t *led_chan;
} LedParams;

bool check_wifi_status()
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA)
    {
        wifi_ap_record_t ap_info;
        esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
        if (err == ESP_OK)
        {
            return true;
        }
    }
    return false;
}

// Pomoćna funkcija za dobijanje širine karaktera iz ArcadePix fonta
uint8_t get_char_width(char c)
{
    if (c < 32 || c > 127)
        return 0; // Radi samo sa ASCII znakovima koji se mogu ispisati

    // Prvi element u podacima svakog karaktera je njegova širina
    int char_index = c - 32;
    int data_index = 0;

    // Izračunaj početnu tačku za karakter u nizu fonta
    for (int i = 0; i < char_index; i++)
    {
        // Svaki karakter koristi 19 bajtova u nizu
        data_index += 19;
    }

    // Vrati širinu zahtijevanog karaktera
    return Arcadepix9x11[data_index];
}

// Pomoćna funkcija za mjerenje širine teksta
uint16_t measure_text(const char *text)
{
    uint16_t width = 0;

    while (*text)
    {
        width += get_char_width(*text) + FONT_SPACING;
        text++;
    }

    // Ukloni poslednji razmak ako je postojao barem jedan karakter
    if (width > 0)
        width -= FONT_SPACING;

    return width;
}

// Pomoćna funkcija za crtanje jednog karaktera iz ArcadePix fonta
void draw_char(uint8_t *bitmap, int x, int y, char c)
{
    if (c < 32 || c > 127)
        return; // Radi samo sa ASCII znakovima koji se mogu ispisati

    // Pronađi karakter u podacima fonta
    int char_index = c - 32;
    int data_index = 0;

    // Pronađi početni indeks za karakter u nizu fonta
    for (int i = 0; i < char_index; i++)
    {
        data_index += 19; // Svaki karakter koristi 19 bajtova u nizu
    }

    // Dobij širinu karaktera
    uint8_t width = Arcadepix9x11[data_index];
    data_index++; // Pomjeri se na prvi bajt podataka nakon širine

    // Nacrtaj bitmap karaktera
    for (int col = 0; col < width; col++)
    {
        for (int byte_row = 0; byte_row < ((FONT_HEIGHT + 7) / 8); byte_row++)
        {
            uint16_t byte_value = Arcadepix9x11[data_index + col * 2 + byte_row];

            for (int bit = 0; bit < 8 && (byte_row * 8 + bit) < FONT_HEIGHT; bit++)
            {
                if (byte_value & (1 << bit))
                {
                    // Postavi piksel na poziciju (x+col, y+byte_row*8+bit)
                    int pixel_x = x + col;
                    int pixel_y = y + byte_row * 8 + bit;

                    if (pixel_x >= 0 && pixel_x < DISPLAY_WIDTH && pixel_y >= 0 && pixel_y < DISPLAY_HEIGHT)
                    {
                        // Izračunaj poziciju bita u bitmapu
                        int byte_idx = (pixel_y / 8) * DISPLAY_WIDTH + pixel_x;
                        int bit_position = pixel_y % 8;

                        // Postavi bit u bitmapu
                        bitmap[byte_idx] |= (1 << bit_position);
                    }
                }
            }
        }
    }
}

// Pomoćna funkcija za crtanje stringa koristeći ArcadePix font
void draw_string(uint8_t *bitmap, int x, int y, const char *str)
{
    int pos_x = x;

    while (*str)
    {
        draw_char(bitmap, pos_x, y, *str);
        pos_x += get_char_width(*str) + FONT_SPACING;
        str++;
    }
}

// Deklaracija za zadatak displeja
void display_task(void *arg);

// Funkcija za slanje podataka igre na NTFY endpoint
void send_game_data(const char *message)
{
    esp_http_client_config_t config = {
        .url = NTFY_ENDPOINT,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, message, strlen(message));

    const int max_retries = 3;
    const int retry_delay_ms = 2000; // 2 sekunde
    int attempt = 0;
    esp_err_t err;

    do
    {
        err = esp_http_client_perform(client);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "HTTP POST Status = %d", esp_http_client_get_status_code(client));
            break;
        }
        else
        {
            ESP_LOGE(TAG, "HTTP POST request failed (attempt %d): %s", attempt + 1, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
        }
        attempt++;
    } while (attempt < max_retries);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP POST request failed after %d attempts", max_retries);
    }

    esp_http_client_cleanup(client);
}

// Ažuriraj network_task da handleuje formatiranim porukama
void network_task(void *arg)
{
    char message[256];

    while (1)
    {
        if (xQueueReceive(network_queue, &message, portMAX_DELAY))
        {
            send_game_data(message);
        }
    }
}

void show_led(uint8_t r, uint8_t g, uint8_t b, uint8_t w, rmt_encoder_handle_t *led_encoder, rmt_channel_handle_t *led_chan)
{
    // Postavi boju piksela
    for (int i = 0; i < NUMBER_OF_LEDS; i++)
    {
        led_strip_pixels[i * 4] = g;
        led_strip_pixels[i * 4 + 1] = r;
        led_strip_pixels[i * 4 + 2] = b;
        led_strip_pixels[i * 4 + 3] = w;
    }

    // Konfiguracija za slanje LED podataka
    rmt_transmit_config_t tx_config = {
        .loop_count = 0, // nema petlje prenosa
    };

    // Pošalji podatke LED-ima
    rmt_transmit(*led_chan, *led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
    rmt_tx_wait_all_done(*led_chan, portMAX_DELAY);
}

void set_buzzer(bool on)
{
    gpio_set_level(BUZZER_PIN, on ? 1 : 0);
}

// Zadatak buzzera koji čeka događaje
void buzzer_task(void *arg)
{
    while (1)
    {
        // Provjeri stanje igre i stanje buzzera
        if (game_state == GAME_OFF)
        {
            buzzer_state = BUZZER_OFF;
            set_buzzer(false);
            end_game_beep_done = false; // Resetuj zastavicu kada je igra isključena
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (game_state == GAME_FINISHED && !end_game_beep_done)
        {
            buzzer_state = BUZZER_FINISHED;
            set_buzzer(true);

            // Instead of one long delay, use shorter delays and check game state
            int beep_time_ms = 0;
            const int check_interval_ms = 100;
            while (beep_time_ms < 10000 && game_state == GAME_FINISHED)
            {
                vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
                beep_time_ms += check_interval_ms;
            }

            set_buzzer(false);
            end_game_beep_done = true; // Mark that we've done the end-game beep
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (game_state == GAME_PLAYING && buzzer_state != BUZZER_SECONDS)
        {
            buzzer_state = BUZZER_SECONDS;
            end_game_beep_done = false; // Reset the flag when game is playing
        }

        switch (buzzer_state)
        {
        case BUZZER_OFF:
            set_buzzer(false);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case BUZZER_SECONDS:
            set_buzzer(true);
            vTaskDelay(pdMS_TO_TICKS(30)); // Buzz for 10ms
            set_buzzer(false);
            vTaskDelay(pdMS_TO_TICKS(990)); // Wait for 990ms (total 1s)
            break;

        default:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

// Helper function to set LED color based on team
void set_led_color(int index, TeamColor team_color)
{
    if (team_color == LEFT_RED)
    {
        led_strip_pixels[index * 4] = 0;       // Green
        led_strip_pixels[index * 4 + 1] = 255; // Red
        led_strip_pixels[index * 4 + 2] = 0;   // Blue
        led_strip_pixels[index * 4 + 3] = 0;   // White
    }
    else if (team_color == RIGHT_BLUE)
    {
        led_strip_pixels[index * 4] = 0;       // Green
        led_strip_pixels[index * 4 + 1] = 0;   // Red
        led_strip_pixels[index * 4 + 2] = 255; // Blue
        led_strip_pixels[index * 4 + 3] = 0;   // White
    }
}

// Helper function to set alternate color pattern
void set_alternate_color(int index, TeamColor team_color)
{
    if ((index % 3) == 0 || (index % 3) == 1)
    {
        led_strip_pixels[index * 4] = 0;       // Green
        led_strip_pixels[index * 4 + 1] = 0;   // Red
        led_strip_pixels[index * 4 + 2] = 0;   // Blue
        led_strip_pixels[index * 4 + 3] = 255; // White
    }
    else
    {
        set_led_color(index, team_color);
    }
}

/**
 * LED zadatak
 * - Ako je igra isključena, ugasi LED
 * - Ako se igra igra (lol), prikaži boju tima kao traku napretka (s bijelom pozadinom)
 * - Ako je igra završena, prikaži boju pobjedničkog tima
 */
void led_task(void *arg)
{
    rmt_encoder_handle_t *led_encoder = ((LedParams *)arg)->led_encoder;
    rmt_channel_handle_t *led_chan = ((LedParams *)arg)->led_chan;

    while (1)
    {
        if (game_state == GAME_OFF)
        {
            show_led(0, 0, 0, 0, led_encoder, led_chan); // Iskljuciti LED
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Ako je igra završena, prikaži pobjedničku boju
        if (game_state == GAME_FINISHED)
        {
            if (team_color == LEFT_RED)
            {
                show_led(255, 0, 0, 0, led_encoder, led_chan); // Prikazi crvenu boju
            }
            else if (team_color == RIGHT_BLUE)
            {
                show_led(0, 0, 255, 0, led_encoder, led_chan); // Prikazi plavu boju
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Ako se igra igra, prikaži boju tima kao neki loading bar (s bijelom pozadinom)
        int led_count = (current_game_time * NUMBER_OF_LEDS) / game_time_seconds;
        memset(led_strip_pixels, 0, sizeof(led_strip_pixels));

        for (int i = 0; i < NUMBER_OF_LEDS; i++)
        {
            if (i < led_count)
            {
                set_led_color(i, team_color);
            }
            else
            {
                set_alternate_color(i, team_color);
            }
        }

        rmt_transmit_config_t tx_config = {
            .loop_count = 0,
        };
        rmt_transmit(*led_chan, *led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
        rmt_tx_wait_all_done(*led_chan, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * Display task - Ažurira SSD1306 OLED displej sa statusom igre
 * - Ako je igra isključena: "Pritisnite za početak"
 * - Ako se igra igra: Prikazuje timer i trenutno pobjednički tim
 * - Ako je igra završena: Prikazuje pobjednika
 */
void display_task(void *arg)
{
    // Buffer za prikaz sadržaja
    uint8_t bitmap[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8] = {0};

    while (1)
    {
        // Očisti bitmapu
        memset(bitmap, 0, sizeof(bitmap));

        // Napravi sadržaj za prikaz na osnovi stanja igre
        if (game_state == GAME_OFF)
        {
            draw_string(bitmap, 16, 40, "Press to Start");
            // Prikaži status Wi-Fi-ja
            draw_string(bitmap, 50, 10, "Wi-Fi");
            if (check_wifi_status())
            {
                draw_string(bitmap, 30, 20, "Connected");
            }
            else
            {
                draw_string(bitmap, 20, 20, "Disconnected");
            }
        }
        else if (game_state == GAME_PLAYING)
        {
            // Prikaži timer i trenutno pobjednički tim
            char status_line[32];
            // Izračunaj preostalo vrijeme za brojanje unazad
            int remaining_time = game_time_seconds - current_game_time;
            int hours = remaining_time / 3600;
            int minutes = (remaining_time % 3600) / 60;
            int seconds = remaining_time % 60;
            char time_formatted[16] = "";
            if (hours > 0)
            {
                snprintf(time_formatted + strlen(time_formatted), sizeof(time_formatted) - strlen(time_formatted), "%dh", hours);
            }
            if (minutes > 0 || hours > 0)
            {
                snprintf(time_formatted + strlen(time_formatted), sizeof(time_formatted) - strlen(time_formatted), "%dm", minutes);
            }
            snprintf(time_formatted + strlen(time_formatted), sizeof(time_formatted) - strlen(time_formatted), "%ds", seconds);

            snprintf(status_line, sizeof(status_line), "Time: %s", time_formatted);
            draw_string(bitmap, 10, 20, status_line);

            const char *winner_text = (team_color == LEFT_RED) ? "RED" : (team_color == RIGHT_BLUE) ? "BLUE"
                                                                                                    : "NONE";

            // Prikaži trenutno pobjednički tim
            char winning_line[32];
            snprintf(winning_line, sizeof(winning_line), "Currently: %s", winner_text);
            draw_string(bitmap, 10, 40, winning_line);
        }
        else if (game_state == GAME_FINISHED)
        {
            // Prikaži pobjednika
            char finish_line[32];
            const char *winner_text = (team_color == LEFT_RED) ? "RED" : (team_color == RIGHT_BLUE) ? "BLUE"
                                                                                                    : "NONE";

            snprintf(finish_line, sizeof(finish_line), "Finished: %s wins!", winner_text);
            draw_string(bitmap, 10, 30, finish_line);
        }

        // Ažuriraj displej sa našom bitmapom
        esp_lcd_panel_draw_bitmap(display_panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, bitmap);

        // Ažuriraj na razumnom nivou
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Pomoćna funkcija za formatiranje vremena kao string
void format_time(int remaining_time, char *buffer, size_t buffer_size)
{
    int minutes = remaining_time / 60;
    int seconds = remaining_time % 60;
    snprintf(buffer, buffer_size, "%02d:%02d", minutes, seconds);
}

// Handler za pritisak tipke (oba tipka)
void IRAM_ATTR button_isr_handler(void *arg)
{
    // Obrada pritisaka tipke
    int pin = (int)arg;

    // Ako je igra završena, resetiraj igru
    if (game_state == GAME_FINISHED)
    {
        game_state = GAME_OFF;
        team_color = NONE;
        current_game_time = 0;
        // send_game_data WITH GAME OVER: Winner <team>
        char end_message[256];
        snprintf(end_message, sizeof(end_message), "GAME OVER: %s has won!",
                 team_color == LEFT_RED ? "RED" : "BLUE");
        xQueueSend(network_queue, &end_message, portMAX_DELAY);

        return;
    }

    // Ako je igra isključena, pokreni igru i postavi boju tima
    if (game_state == GAME_OFF)
    {
        game_state = GAME_PLAYING;
        current_game_time = 0; // Resetiraj vrijeme kada se pokrene igra
        buzzer_state = BUZZER_SECONDS;
        if (pin == LEFT_BUTTON_PIN)
        {
            team_color = LEFT_RED;
        }
        else if (pin == RIGHT_BUTTON_PIN)
        {
            team_color = RIGHT_BLUE;
        }

        char message[256];
        snprintf(message, sizeof(message), "%s took the hill. GAME STARTED!",
                 team_color == LEFT_RED ? "RED" : "BLUE");
        xQueueSend(network_queue, &message, portMAX_DELAY);
        return;
    }

    // Inače samo postavi boju tima
    if (game_state == GAME_PLAYING)
    {
        // Guard statement ako trenutni tim pritisne svoju tipku
        if (team_color == LEFT_RED && pin == LEFT_BUTTON_PIN)
        {
            return;
        }
        else if (team_color == RIGHT_BLUE && pin == RIGHT_BUTTON_PIN)
        {
            return;
        }

        if (pin == LEFT_BUTTON_PIN)
        {
            team_color = LEFT_RED;
        }
        else if (pin == RIGHT_BUTTON_PIN)
        {
            team_color = RIGHT_BLUE;
        }
        // send_game_data sa trenutnom bojom tima
        char message[256];
        char time_buffer[16];
        format_time(game_time_seconds - current_game_time, time_buffer, sizeof(time_buffer));
        snprintf(message, sizeof(message), "%s took the hill! Time left: %s",
                 team_color == LEFT_RED ? "RED" : "BLUE", time_buffer);
        xQueueSend(network_queue, &message, portMAX_DELAY);
    }
}

// Handler za Wi-Fi događaje
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying connection to Wi-Fi");
    }
}

// Inicijaliziraj Wi-Fi
void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{

    // Ovo je ukradeno
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    ESP_LOGI(TAG, "Create RMT TX channel");
    rmt_channel_handle_t led_chan = NULL;
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT, // select source clock
        .gpio_num = RMT_LED_STRIP_GPIO_NUM,
        .mem_block_symbols = 64, // increase the block size can make the LED less flickering
        .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4, // set the number of transactions that can be pending in the background
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    // Inicijaliziraj RMT TX kanal za LED traku

    ESP_LOGI(TAG, "Install led strip encoder");
    rmt_encoder_handle_t led_encoder = NULL;
    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    ESP_LOGI(TAG, "Enable RMT TX channel");
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    // Inicijaliziraj I2C bus za displej
    ESP_LOGI(TAG, "Initialize I2C bus");
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DISPLAY_SDA_PIN,
        .scl_io_num = DISPLAY_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000, // 400 KHz
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_HOST, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_HOST, I2C_MODE_MASTER, 0, 0, 0));

    // Inicijaliziraj SSD1306 displej
    ESP_LOGI(TAG, "Initialize SSD1306 display");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = DISPLAY_I2C_ADDR,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(I2C_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &display_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(display_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(display_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(display_panel, true));

    // Inicijaliziraj Wi-Fi
    wifi_init();

    // Napravi zadatak za displej
    xTaskCreate(
        display_task,
        "display_task",
        4096, // Veći stack za operacije sa stringovima
        NULL,
        10,
        NULL);

    // Inicijaliziraj GPIO za zvonec
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUZZER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Inicijaliziraj GPIO za tipke
    gpio_config_t button_configs = {
        .pin_bit_mask = (1ULL << LEFT_BUTTON_PIN) | (1ULL << RIGHT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&button_configs);

    // Napravi objekt za zadatak za zvonce
    LedParams led_params = {
        .led_encoder = &led_encoder,
        .led_chan = &led_chan};

    // Napravi zadatak za zvonce s 100ms intervalom
    xTaskCreate(
        (TaskFunction_t)buzzer_task,
        "buzzer_task",
        2048,
        NULL,
        10,
        NULL);

    // Napravi zadatak za LED traku
    xTaskCreate(
        (TaskFunction_t)led_task,
        "led_task",
        2048,
        &led_params,
        10,
        NULL);
    // Napravi red za mrežu i zadatak
    network_queue = xQueueCreate(QUEUE_SIZE, sizeof(char[256]));
    xTaskCreate(network_task, "network_task", 4096, NULL, 10, NULL);

    // Postavi upravljače za prekid tipke
    gpio_install_isr_service(0);
    gpio_isr_handler_add(LEFT_BUTTON_PIN, button_isr_handler, (void *)LEFT_BUTTON_PIN);
    gpio_isr_handler_add(RIGHT_BUTTON_PIN, button_isr_handler, (void *)RIGHT_BUTTON_PIN);

    // Očisti sve pixele
    memset(led_strip_pixels, 0, sizeof(led_strip_pixels));

    // Održavaj program u pokretu
    while (1)
    {
        if (game_state == GAME_PLAYING)
        {
            if (current_game_time == game_time_seconds / 2)
            {
                static char halfway_message[256];
                char time_buffer[16];
                format_time(game_time_seconds - current_game_time, time_buffer, sizeof(time_buffer));
                snprintf(halfway_message, sizeof(halfway_message), "HALFWAY: %s is holding the hill, time left: %s",
                         team_color == LEFT_RED ? "RED" : "BLUE", time_buffer);
                xQueueSend(network_queue, &halfway_message, portMAX_DELAY);
            }
            if (current_game_time < game_time_seconds)
            {
                current_game_time++;
            }
            else
            {
                game_state = GAME_FINISHED;
                static char end_message[256];
                snprintf(end_message, sizeof(end_message), "GAME OVER: %s has won!",
                         team_color == LEFT_RED ? "RED" : "BLUE");
                xQueueSend(network_queue, &end_message, portMAX_DELAY);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
