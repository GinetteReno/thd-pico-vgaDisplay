#ifndef _VGA_H
#define _VGA_H
#include "pico/stdlib.h"

#define VGA_BGR 1

// Length of the pixel array, and number of DMA transfers
#define TXCOUNT 76800 // Total pixels

#if VGA_BGR
#define BLACK 0b0
#define RED 0b100
#define GREEN 0b010
#define YELLOW 0b110
#define BLUE 0b001
#define MAGENTA 0b101
#define CYAN 0b011
#define WHITE 0b111
#else
#define BLACK 0
#define RED 1
#define GREEN 2
#define YELLOW 3
#define BLUE 4
#define MAGENTA 5
#define CYAN 6
#define WHITE 7
#endif
void VGA_writePixel(int x, int y, char color);
void VGA_initDisplay(uint vsync_pin, uint hsync_pin, uint r_pin);

void VGA_fillScreen(uint16_t color);

void VGA_drawFrame(void *src);

void dma_memset(void *dest, uint8_t val, size_t num);
void dma_memcpy(void *dest, void *src, size_t num);
#endif