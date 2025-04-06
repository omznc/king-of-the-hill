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

// Constants
#define LEFT_BUTTON_PIN      47
#define RIGHT_BUTTON_PIN     21
#define BUZZER_PIN           46
#define LED_STRIP_PIN         3
#define DISPLAY_SDA_PIN      48  // Display data pin
#define DISPLAY_SCL_PIN      45  // Display clock pin

#define FONT_HEIGHT          11
#define FONT_SPACING         1
#define DISPLAY_WIDTH        128
#define DISPLAY_HEIGHT       64

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution
#define RMT_LED_STRIP_GPIO_NUM      3
#define NUMBER_OF_LEDS              40

#define I2C_HOST               0
#define DISPLAY_I2C_ADDR       0x3C

#define BUZZER_EVENT_BIT       (1 << 0)

// Enums
typedef enum {
    GAME_OFF,
    GAME_PLAYING,
    GAME_FINISHED
} GameState;

typedef enum {
    NONE,
    LEFT_RED,
    RIGHT_BLUE,
} TeamColor;

typedef enum {
    BUZZER_OFF,
    BUZZER_SECONDS,
    BUZZER_FINISHED
} BuzzerState;

// Global Variables
static BuzzerState buzzer_state = BUZZER_OFF;
static GameState game_state = GAME_OFF;
static TeamColor team_color = NONE;
static const int game_time_seconds = 900; // 15 minutes
static int current_game_time = 0;
static esp_lcd_panel_handle_t display_panel = NULL;
static uint8_t led_strip_pixels[NUMBER_OF_LEDS * 4];
static bool end_game_beep_done = false;
static const char *TAG = "king-of-the-hill";

// External reference to the font data
extern const unsigned short Arcadepix9x11[];

// Structs
typedef struct LedParams {
    rmt_encoder_handle_t* led_encoder;
    rmt_channel_handle_t* led_chan;
} LedParams;

// Helper function to get character width from ArcadePix font
uint8_t get_char_width(char c) {
    if (c < 32 || c > 127) return 0; // Only handle printable ASCII
    
    // The first element in each character's data is its width
    int char_index = c - 32;
    int data_index = 0;
    
    // Calculate the starting point for the character in the font array
    for (int i = 0; i < char_index; i++) {
        // Each character uses (width + 1) * 19 bytes in the array
        data_index += 19;
    }
    
    // Return the width of the requested character
    return Arcadepix9x11[data_index];
}

// Helper function to measure text width
uint16_t measure_text(const char* text) {
    uint16_t width = 0;
    
    while (*text) {
        width += get_char_width(*text) + FONT_SPACING;
        text++;
    }
    
    // Remove the last spacing if there was at least one character
    if (width > 0) width -= FONT_SPACING;
    
    return width;
}

// Helper function to draw a single character from ArcadePix font
void draw_char(uint8_t* bitmap, int x, int y, char c) {
    if (c < 32 || c > 127) return; // Only handle printable ASCII
    
    // Find character in font data
    int char_index = c - 32;
    int data_index = 0;
    
    // Find the starting index for the character in the font array
    for (int i = 0; i < char_index; i++) {
        data_index += 19; // Each char uses 19 bytes in the array
    }
    
    // Get the width of the character
    uint8_t width = Arcadepix9x11[data_index];
    data_index++; // Move to the first data byte after the width
    
    // Draw the character bitmap
    for (int col = 0; col < width; col++) {
        for (int byte_row = 0; byte_row < ((FONT_HEIGHT + 7) / 8); byte_row++) {
            uint16_t byte_value = Arcadepix9x11[data_index + col * 2 + byte_row];
            
            for (int bit = 0; bit < 8 && (byte_row * 8 + bit) < FONT_HEIGHT; bit++) {
                if (byte_value & (1 << bit)) {
                    // Set pixel at position (x+col, y+byte_row*8+bit)
                    int pixel_x = x + col;
                    int pixel_y = y + byte_row * 8 + bit;
                    
                    if (pixel_x >= 0 && pixel_x < DISPLAY_WIDTH && pixel_y >= 0 && pixel_y < DISPLAY_HEIGHT) {
                        // Calculate bit position in the bitmap
                        int byte_idx = (pixel_y / 8) * DISPLAY_WIDTH + pixel_x;
                        int bit_position = pixel_y % 8;
                        
                        // Set the bit in the bitmap
                        bitmap[byte_idx] |= (1 << bit_position);
                    }
                }
            }
        }
    }
}

// Helper function to draw a string using ArcadePix font
void draw_string(uint8_t* bitmap, int x, int y, const char* str) {
    int pos_x = x;
    
    while (*str) {
        draw_char(bitmap, pos_x, y, *str);
        pos_x += get_char_width(*str) + FONT_SPACING;
        str++;
    }
}


// Forward declaration for display task
void display_task(void *arg);

void show_led(uint8_t r, uint8_t g, uint8_t b, uint8_t w, rmt_encoder_handle_t* led_encoder, rmt_channel_handle_t* led_chan)
{
    // Set the pixel color
    for (int i = 0; i < NUMBER_OF_LEDS; i++) {
        led_strip_pixels[i * 4] = g;
        led_strip_pixels[i * 4 + 1] = r;
        led_strip_pixels[i * 4 + 2] = b;
        led_strip_pixels[i * 4 + 3] = w;
    }

    // Configuration for transmitting the LED data
    rmt_transmit_config_t tx_config = {
        .loop_count = 0, // no transfer loop
    };
    
    // Send the data to the LEDs
    rmt_transmit(*led_chan, *led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
    rmt_tx_wait_all_done(*led_chan, portMAX_DELAY);
}

void set_buzzer(bool on)
{
    gpio_set_level(BUZZER_PIN, on ? 1 : 0);
}

// Buzzer task that waits for events
void buzzer_task(void* arg) {
    while (1) {
        // Check the game state and buzzer state
        if (game_state == GAME_OFF) {
            buzzer_state = BUZZER_OFF;
            set_buzzer(false);
            end_game_beep_done = false; // Reset the flag when game is off
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        if (game_state == GAME_FINISHED && !end_game_beep_done) {
            buzzer_state = BUZZER_FINISHED;
            set_buzzer(true);
            
            // Instead of one long delay, use shorter delays and check game state
            int beep_time_ms = 0;
            const int check_interval_ms = 100;
            while (beep_time_ms < 10000 && game_state == GAME_FINISHED) {
                vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
                beep_time_ms += check_interval_ms;
            }
            
            set_buzzer(false);
            end_game_beep_done = true; // Mark that we've done the end-game beep
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        if (game_state == GAME_PLAYING && buzzer_state != BUZZER_SECONDS) {
            buzzer_state = BUZZER_SECONDS;
            end_game_beep_done = false; // Reset the flag when game is playing
        }

        switch (buzzer_state) {
            case BUZZER_OFF:
                set_buzzer(false);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            
            case BUZZER_SECONDS:
                set_buzzer(true);
                vTaskDelay(pdMS_TO_TICKS(10)); // Buzz for 10ms
                set_buzzer(false);
                vTaskDelay(pdMS_TO_TICKS(990)); // Wait for 990ms (total 1s)
                break;

            default:
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}

/**
 * LED task
 * - If the game is off, turn off the LED
 * - If the game is playing, show the team color as a progress bar (with white background)
 * - If the game is finished, show the winning team color
 */
void led_task(void* arg) {
    rmt_encoder_handle_t* led_encoder = ((LedParams*)arg)->led_encoder;
    rmt_channel_handle_t* led_chan = ((LedParams*)arg)->led_chan;

    while (1) {
        if (game_state == GAME_OFF) {
            show_led(0, 0, 0, 0, led_encoder, led_chan); // Turn off LED
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // If the game is finished, show the winning team
        if (game_state == GAME_FINISHED) {
            if (team_color == LEFT_RED) {
                show_led(255, 0, 0, 0, led_encoder, led_chan); // Show red
            } else if (team_color == RIGHT_BLUE) {
                show_led(0, 0, 255, 0, led_encoder, led_chan); // Show blue
            }
            vTaskDelay(pdMS_TO_TICKS(100)); // Keep updating at a reasonable rate
            continue; // Continue the loop rather than breaking
        }
        
        // If the game is playing, show the team color as a progress bar
        int led_count = (current_game_time * NUMBER_OF_LEDS) / game_time_seconds;
        memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
        for (int i = 0; i < led_count; i++) {
            if (team_color == LEFT_RED) {
                led_strip_pixels[i * 4] = 0; // Green
                led_strip_pixels[i * 4 + 1] = 255; // Red
                led_strip_pixels[i * 4 + 2] = 0; // Blue
                led_strip_pixels[i * 4 + 3] = 0; // White
            } else if (team_color == RIGHT_BLUE) {
                led_strip_pixels[i * 4] = 0; // Green
                led_strip_pixels[i * 4 + 1] = 0; // Red
                led_strip_pixels[i * 4 + 2] = 255; // Blue
                led_strip_pixels[i * 4 + 3] = 0; // White
            }
        }
        
        // Show the LED strip
        rmt_transmit_config_t tx_config = {
            .loop_count = 0, // no transfer loop
        };
        rmt_transmit(*led_chan, *led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config);
        rmt_tx_wait_all_done(*led_chan, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(100)); // Update at higher frequency for smooth progress bar
    }
}

/**
 * Display task - Updates the SSD1306 OLED display with game status
 * - If game is off: "Press to Start"
 * - If game is playing: Shows timer and current winning team
 * - If game is finished: Shows winner
 */
void display_task(void *arg) {
    // Buffer for display content
    uint8_t bitmap[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8] = {0};
    
    while (1) {
        // Clear the bitmap
        memset(bitmap, 0, sizeof(bitmap));
        
        // Create display content based on game state
        if (game_state == GAME_OFF) {
            // Show "Press to Start"
            draw_string(bitmap, 16, 28, "Press to Start");
        } 
        else if (game_state == GAME_PLAYING) {
            // Show timer and current winning team
            char status_line[32];
            // Calculate remaining time for countdown
            int remaining_time = game_time_seconds - current_game_time;
            int hours = remaining_time / 3600;
            int minutes = (remaining_time % 3600) / 60;
            int seconds = remaining_time % 60;
            char time_formatted[16] = "";
            if (hours > 0) {
                snprintf(time_formatted + strlen(time_formatted), sizeof(time_formatted) - strlen(time_formatted), "%dh", hours);
            }
            if (minutes > 0 || hours > 0) {
                snprintf(time_formatted + strlen(time_formatted), sizeof(time_formatted) - strlen(time_formatted), "%dm", minutes);
            }
            snprintf(time_formatted + strlen(time_formatted), sizeof(time_formatted) - strlen(time_formatted), "%ds", seconds);

            snprintf(status_line, sizeof(status_line), "Time: %s", time_formatted);
            draw_string(bitmap, 10, 20, status_line);
            
            const char *winner_text = (team_color == LEFT_RED) ? "RED" : 
                                     (team_color == RIGHT_BLUE) ? "BLUE" : "NONE";
            
            // Show currently winning team
            char winning_line[32];
            snprintf(winning_line, sizeof(winning_line), "Currently: %s", winner_text);
            draw_string(bitmap, 10, 40, winning_line);
        } 
        else if (game_state == GAME_FINISHED) {
            // Show winner
            char finish_line[32];
            const char *winner_text = (team_color == LEFT_RED) ? "RED" : 
                                     (team_color == RIGHT_BLUE) ? "BLUE" : "NONE";
            
            snprintf(finish_line, sizeof(finish_line), "Finished: %s wins!", winner_text);
            draw_string(bitmap, 10, 30, finish_line);
        }
        
        // Update the display with our bitmap
        esp_lcd_panel_draw_bitmap(display_panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, bitmap);
        
        // Update at a reasonable rate
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Button interrupt handler (both buttons)
void IRAM_ATTR button_isr_handler(void* arg) {
    // Handle button press
    int pin = (int)arg;

    // If the game is finished, reset the game
    if (game_state == GAME_FINISHED) {
        game_state = GAME_OFF;
        team_color = NONE;
        current_game_time = 0;
        return;
    }
    
    // If the game is stopped, start it and set the team color
    if (game_state == GAME_OFF) {
        game_state = GAME_PLAYING;
        current_game_time = 0; // Reset the timer when starting
        buzzer_state = BUZZER_SECONDS;
        if (pin == LEFT_BUTTON_PIN) {
            team_color = LEFT_RED;
        } else if (pin == RIGHT_BUTTON_PIN) {
            team_color = RIGHT_BLUE;
        }
        return;
    }

    // Otherwise just set the team color
    if (game_state == GAME_PLAYING) {
        if (pin == LEFT_BUTTON_PIN) {
            team_color = LEFT_RED;
        } else if (pin == RIGHT_BUTTON_PIN) {
            team_color = RIGHT_BLUE;
        }
    }
}

void app_main(void)
{
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

    ESP_LOGI(TAG, "Install led strip encoder");
    rmt_encoder_handle_t led_encoder = NULL;
    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    ESP_LOGI(TAG, "Enable RMT TX channel");
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    // Initialize I2C bus for the display
    ESP_LOGI(TAG, "Initialize I2C bus");
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DISPLAY_SDA_PIN,
        .scl_io_num = DISPLAY_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,  // 400 KHz
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_HOST, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_HOST, I2C_MODE_MASTER, 0, 0, 0));

    // Initialize the SSD1306 display
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

    // Create the display task
    xTaskCreate(
        display_task,
        "display_task",
        4096,  // Larger stack size for string operations
        NULL,
        10,
        NULL
    );

    // Initialize the buzzer GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUZZER_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Initialize the button GPIOs
    gpio_config_t button_configs = {
        .pin_bit_mask = (1ULL << LEFT_BUTTON_PIN) | (1ULL << RIGHT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&button_configs);

    // create a config object for the buzzer task
    LedParams led_params = {
        .led_encoder = &led_encoder,
        .led_chan = &led_chan
    };
    
    // Create the buzzer task with a 100ms buzz interval
    xTaskCreate(
        (TaskFunction_t)buzzer_task,
        "buzzer_task",
        2048,
        NULL,
        10,
        NULL
    );

    // Create the LED task
    xTaskCreate(
        (TaskFunction_t)led_task,
        "led_task",
        2048,
        &led_params,
        10,
        NULL
    );

    // Set up the button interrupt handlers
    gpio_install_isr_service(0);
    gpio_isr_handler_add(LEFT_BUTTON_PIN, button_isr_handler, (void*)LEFT_BUTTON_PIN);
    gpio_isr_handler_add(RIGHT_BUTTON_PIN, button_isr_handler, (void*)RIGHT_BUTTON_PIN);
    
    // Clear all pixels first
    memset(led_strip_pixels, 0, sizeof(led_strip_pixels));

    // Keep the program running
    while (1) {
        if (game_state == GAME_PLAYING) {  // Fixed using == instead of =
            if (current_game_time < game_time_seconds) {
                current_game_time++;
            } else {
                game_state = GAME_FINISHED;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
