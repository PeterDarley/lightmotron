#ifndef MAX7219_H
#define MAX7219_H

#include "esp_err.h"
#include "driver/spi_master.h"
#include <stdint.h>

/**
 * MAX7219 8x8 LED matrix driver (SPI).
 *
 * Supports chaining multiple 8x8 modules.
 */
typedef struct {
    spi_device_handle_t spi;
    int num_modules;
    uint8_t *framebuffer; /* num_modules * 8 bytes */
    int cs_pin;
    bool initialized;
} max7219_t;

/**
 * Initialize MAX7219 chain on SPI.
 */
esp_err_t max7219_init(max7219_t *dev, int mosi_pin, int sck_pin, int cs_pin, int num_modules);

/**
 * Set brightness (0-15) on all modules.
 */
esp_err_t max7219_set_brightness(max7219_t *dev, int brightness);

/**
 * Clear framebuffer and push to display.
 */
void max7219_clear(max7219_t *dev);

/**
 * Set a pixel in the framebuffer.
 */
void max7219_set_pixel(max7219_t *dev, int x, int y, int val);

/**
 * Push framebuffer to the hardware.
 */
esp_err_t max7219_show(max7219_t *dev);

/**
 * Draw a character at pixel position (x, y) using built-in 8x8 font.
 */
void max7219_draw_char(max7219_t *dev, char c, int x, int y);

/**
 * Draw text string at position.
 */
void max7219_draw_text(max7219_t *dev, const char *text, int x, int y);

#endif /* MAX7219_H */
