/**
 * Hunter Adams (vha3@cornell.edu)
 *
 * VGA driver using PIO assembler
 *
 * HARDWARE CONNECTIONS
 *  - GPIO 17 ---> VGA Hsync
 *  - GPIO 16 ---> VGA Vsync
 *  - GPIO 18 ---> 330 ohm resistor ---> VGA Red
 *  - GPIO 19 ---> 330 ohm resistor ---> VGA Green
 *  - GPIO 20 ---> 330 ohm resistor ---> VGA Blue
 *  - RP2040 GND ---> VGA GND
 *
 * RESOURCES USED
 *  - PIO state machines 0, 1, and 2 on PIO instance 0
 *  - DMA channels 0 and 1
 *  - 153.6 kBytes of RAM (for pixel color data)
 *
 *
 * Modified by Vlad Tomoiaga (tvlad1234) to be used as a library
 *
 */

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"

#include "vga.h"

// #include "hsync.pio.h"
// #include "vsync.pio.h"
// #include "rgb.pio.h"
#include "../thd_hsync.pio.h"
#include "../thd_vsync.pio.h"
#include "../thd_rgb.pio.h"
#include "../thd_pclk.pio.h"

#define CLK_PULSE 10
#define H_ACTIVE 339   // (active + frontporch - 1) - one cycle delay for mov
#define V_ACTIVE_PLUS_FRONT 243   // (active - 1)

#define RGB_ACTIVE 319 // change to this if 1 pixel/byte

uint16_t _width = 320;
uint16_t _height = 240;

// Pixel color array that is DMA's to the PIO machines and
// a pointer to the ADDRESS of this color array.
// Note that this array is automatically initialized to all 0's (black)
unsigned char vga_data_array[TXCOUNT];
char *address_pointer = &vga_data_array[0];

// DMA channel for dma_memcpy and dma_memset
int memcpy_dma_chan;

void VGA_initDisplay(uint vsync_pin, uint hsync_pin, uint r_pin, uint pclk_pin)
{
    // Choose which PIO instance to use (there are two instances, each with 4 state machines)
    PIO pio = pio1;

    // Our assembled program needs to be loaded into this PIO's instruction
    // memory. This SDK function will find a location (offset) in the
    // instruction memory where there is enough space for our program. We need
    // to remember these locations!
    //
    // We only have 32 instructions to spend! If the PIO programs contain more than
    // 32 instructions, then an error message will get thrown at these lines of code.
    //
    // The program name comes from the .program part of the pio file
    // and is of the form <program name_program>
    uint hsync_offset = pio_add_program(pio, &thd_hsync_program);
    uint vsync_offset = pio_add_program(pio, &thd_vsync_program);
    //uint pclk_offset = pio_add_program(pio, &thd_pclk_program);
    uint rgb_offset = pio_add_program(pio, &thd_rgb_program);
    

    // Manually select a few state machines from pio instance pio0.
    uint hsync_sm = 0;
    uint vsync_sm = 1;
    uint pclk_sm = 2;
    uint rgb_sm = 3;
    

    // Call the initialization functions that are defined within each PIO file.
    // Why not create these programs here? By putting the initialization function in
    // the pio file, then all information about how to use/setup that state machine
    // is consolidated in one place. Here in the C, we then just import and use it.
    hsync_program_init(pio, hsync_sm, hsync_offset, hsync_pin);
    vsync_program_init(pio, vsync_sm, vsync_offset, vsync_pin);
    //pclk_program_init(pio, pclk_sm, pclk_offset, pclk_pin);
    rgb_program_init(pio, rgb_sm, rgb_offset, r_pin);
    

    /////////////////////////////////////////////////////////////////////////////////////////////////////
    // ===========================-== DMA Data Channels =================================================
    /////////////////////////////////////////////////////////////////////////////////////////////////////

    // DMA channels - 0 sends color data, 1 reconfigures and restarts 0
    int rgb_chan_0 = dma_claim_unused_channel(true);
    int rgb_chan_1 = dma_claim_unused_channel(true);

    // DMA channel for dma_memcpy and dma_memset
    memcpy_dma_chan = dma_claim_unused_channel(true);

    // Channel Zero (sends color data to PIO VGA machine)
    dma_channel_config c0 = dma_channel_get_default_config(rgb_chan_0); // default configs
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_8);             // 8-bit txfers
    channel_config_set_read_increment(&c0, true);                       // yes read incrementing
    channel_config_set_write_increment(&c0, false);                     // no write incrementing
    if (pio == pio0)
        channel_config_set_dreq(&c0, DREQ_PIO0_TX2); // DREQ_PIO0_TX2 pacing (FIFO)
    else
        channel_config_set_dreq(&c0, DREQ_PIO1_TX2); // DREQ_PIO1_TX2 pacing (FIFO)
    channel_config_set_chain_to(&c0, rgb_chan_1);    // chain to other channel

    dma_channel_configure(
        rgb_chan_0,        // Channel to be configured
        &c0,               // The configuration we just created
        &pio->txf[rgb_sm], // write address (RGB PIO TX FIFO)
        &vga_data_array,   // The initial read address (pixel color array)
        TXCOUNT,           // Number of transfers; in this case each is 1 byte.
        false              // Don't start immediately.
    );

    // Channel One (reconfigures the first channel)
    dma_channel_config c1 = dma_channel_get_default_config(rgb_chan_1); // default configs
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);            // 32-bit txfers
    channel_config_set_read_increment(&c1, false);                      // no read incrementing
    channel_config_set_write_increment(&c1, false);                     // no write incrementing
    channel_config_set_chain_to(&c1, rgb_chan_0);                       // chain to other channel

    dma_channel_configure(
        rgb_chan_1,                        // Channel to be configured
        &c1,                               // The configuration we just created
        &dma_hw->ch[rgb_chan_0].read_addr, // Write address (channel 0 read address)
        &address_pointer,                  // Read address (POINTER TO AN ADDRESS)
        1,                                 // Number of transfers, in this case each is 4 byte
        false                              // Don't start immediately.
    );

    /////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////////////

    // Initialize PIO state machine counters. This passes the information to the state machines
    // that they retrieve in the first 'pull' instructions, before the .wrap_target directive
    // in the assembly. Each uses these values to initialize some counting registers.
    pio_sm_put_blocking(pio, hsync_sm, H_ACTIVE);
    pio_sm_put_blocking(pio, vsync_sm, 239);//V_ACTIVE_PLUS_FRONT);
    pio_sm_put_blocking(pio, rgb_sm, RGB_ACTIVE);

    // Start the two pio machine IN SYNC
    // Note that the RGB state machine is running at full speed,
    // so synchronization doesn't matter for that one. But, we'll
    // start them all simultaneously anyway.
    pio_enable_sm_mask_in_sync(pio, ((1u << hsync_sm) | (1u << vsync_sm) | (1u << rgb_sm)) | (1u << pclk_sm));

    // Start DMA channel 0. Once started, the contents of the pixel color array
    // will be continously DMA's to the PIO machines that are driving the screen.
    // To change the contents of the screen, we need only change the contents
    // of that array.
    dma_start_channel_mask((1u << rgb_chan_0));
}

void dma_memset(void *dest, uint8_t val, size_t num)
{
    dma_channel_config c = dma_channel_get_default_config(memcpy_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);

    dma_channel_configure(
        memcpy_dma_chan, // Channel to be configured
        &c,              // The configuration we just created
        dest,            // The initial write address
        &val,            // The initial read address
        num,             // Number of transfers; in this case each is 1 byte.
        true             // Start immediately.
    );

    // We could choose to go and do something else whilst the DMA is doing its
    // thing. In this case the processor has nothing else to do, so we just
    // wait for the DMA to finish.
    dma_channel_wait_for_finish_blocking(memcpy_dma_chan);
}

void dma_memcpy(void *dest, void *src, size_t num)
{
    dma_channel_config c = dma_channel_get_default_config(memcpy_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    dma_channel_configure(
        memcpy_dma_chan, // Channel to be configured
        &c,              // The configuration we just created
        dest,            // The initial write address
        src,             // The initial read address
        num,             // Number of transfers; in this case each is 1 byte.
        true             // Start immediately.
    );

    // We could choose to go and do something else whilst the DMA is doing its
    // thing. In this case the processor has nothing else to do, so we just
    // wait for the DMA to finish.
    dma_channel_wait_for_finish_blocking(memcpy_dma_chan);
}

void VGA_fillScreen(uint16_t color)
{
    dma_memset(vga_data_array, (color) | (color << 3), TXCOUNT);
}


void VGA_drawFrame(void *src)
{
    dma_memcpy(vga_data_array, src, TXCOUNT);
}