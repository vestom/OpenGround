/*
    Copyright 2016 fishpepper <AT> gmail.com

    This program is free software: you can redistribute it and/ or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http:// www.gnu.org/licenses/>.

    author: fishpepper <AT> gmail.com
*/

#include "cc2500.h"
#include "spi.h"

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>

#include "debug.h"
#include "timeout.h"
#include <string.h>

// internal functions
static void cc2500_init_gpio(void);


#define CC2500_DEBUG_STATUSBYTE 0

void cc2500_init(void) {
    debug("cc2500: init\n"); debug_flush();
    cc2500_init_gpio();
    spi_init();
}

static void cc2500_init_gpio(void) {
    // set high:
    gpio_set(POWERDOWN_GPIO, POWERDOWN_PIN);

    // set powerdown trigger pin as output
    gpio_mode_setup(POWERDOWN_GPIO, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, POWERDOWN_PIN);
    gpio_set_output_options(POWERDOWN_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, POWERDOWN_PIN);

    // PA/ LNA:
    // periph clock enable for port
    rcc_periph_clock_enable(GPIO_RCC(CC2500_LNA_GPIO));
    rcc_periph_clock_enable(GPIO_RCC(CC2500_PA_GPIO));

    // CTX:
    // set all gpio directions to output
    gpio_mode_setup(CC2500_LNA_GPIO, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, CC2500_LNA_PIN);
    gpio_set_output_options(CC2500_LNA_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, CC2500_LNA_PIN);

    // CRX:
    gpio_mode_setup(CC2500_PA_GPIO, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, CC2500_PA_PIN);
    gpio_set_output_options(CC2500_PA_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, CC2500_PA_PIN);

    cc2500_enter_rxmode();

    // enable clock for GDO1
    rcc_periph_clock_enable(GPIO_RCC(CC2500_GDO1_GPIO));

    // setup gdo1
    gpio_mode_setup(CC2500_GDO1_GPIO, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, CC2500_GDO1_PIN);
    gpio_set_output_options(CC2500_GDO1_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, CC2500_GDO1_PIN);

    // periph clock enable for port
    rcc_periph_clock_enable(GPIO_RCC(CC2500_GDO2_GPIO));

    // configure GDO2 pins as Input
    gpio_mode_setup(CC2500_GDO2_GPIO, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, CC2500_GDO2_PIN);
    gpio_set_output_options(CC2500_GDO2_GPIO, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, CC2500_GDO2_PIN);
}

inline void cc2500_enter_rxmode(void) {
    // LNA = 1, PA = 0
    gpio_set(CC2500_LNA_GPIO, CC2500_LNA_PIN);  // 1
    delay_us(20);
    gpio_clear(CC2500_PA_GPIO, CC2500_PA_PIN);     // 0
    delay_us(5);
}

#if 0
inline uint32_t cc2500_set_antenna(uint8_t id) {
    // select antenna 0 or 1:
    if (id) {
        gpio_clear(CC2500_ANT_SW_CTX_GPIO, CC2500_ANT_SW_CTX_PIN);  // 0
        gpio_set(CC2500_ANT_SW_CRX_GPIO, CC2500_ANT_SW_CRX_PIN);  // 1
    } else {
        gpio_set(CC2500_ANT_SW_CTX_GPIO, CC2500_ANT_SW_CTX_PIN);  // 1
        gpio_clear(CC2500_ANT_SW_CRX_GPIO, CC2500_ANT_SW_CRX_PIN);  // 0
    }
    return id;
}
#endif  // 0

inline void cc2500_set_gdo_mode(void) {
    // set to RX FIFO signal
    cc2500_set_register(IOCFG0, 0x01);
    // cc2500_set_register(IOCFG1, 0x02); //
    cc2500_set_register(IOCFG2, 0x02);
}

inline void cc2500_set_register(uint8_t address, uint8_t data) {
    // select device
    cc2500_csn_lo();


    // wait for ready signal
    while (gpio_get(CC2500_SPI_GPIO, CC2500_SPI_MISO_PIN) == 1) {}

    spi_tx(address);
    spi_tx(data);

    // deslect
    cc2500_csn_hi();
}

inline uint8_t cc2500_get_register(uint8_t address) {
    uint8_t result;

    // select device:
    cc2500_csn_lo();

    // wait for RDY signal:
    while (gpio_get(CC2500_SPI_GPIO, CC2500_SPI_MISO_PIN) == 1) {}

    // request address(read request has bit7 set)
    #if CC2500_DEBUG_STATUSBYTE
        uint8_t status = spi_tx(address | 0x80);
        debug("spi status = ");
        debug_put_hex8(status);
        debug_put_newline();
    #else
        spi_tx(address | 0x80);
    #endif  // CC2500_DEBUG_STATUSBYTE

    // fetch result:
    result = spi_rx();

    // deselect device
    cc2500_csn_hi();

    // return result
    return(result);
}

inline void cc2500_strobe(uint8_t address) {
    cc2500_csn_lo();

    #if CC2500_DEBUG_STATUSBYTE
        uint8_t status = spi_tx(address);
        debug("spi status = ");
        debug_put_hex8(status);
        debug_put_newline();
    #else
        spi_tx(address);
    #endif  // CC2500_DEBUG_STATUSBYTE

    // debug("s"); debug_put_hex8(status); debug_put_newline();
    cc2500_csn_hi();
}

inline uint8_t cc2500_get_status(void) {
    cc2500_csn_lo();
    uint8_t status = spi_tx(0xFF);

    #if CC2500_DEBUG_STATUSBYTE
        debug("spi status = ");
        debug_put_hex8(status);
        debug_put_newline();
    #endif  // CC2500_DEBUG_STATUSBYT

    cc2500_csn_hi();
    return status;
}

uint8_t cc2500_transmission_completed(void) {
    // after tx cc25xx goes back to RX(configured by mcsm1 register)
    return((cc2500_get_status() & (0x70)) == CC2500_STATUS_STATE_RX);
}



inline void cc2500_enter_txmode(void) {
    // LNA = 0, PA = 1
    gpio_clear(CC2500_LNA_GPIO, CC2500_LNA_PIN);  // 0
    delay_us(20);
    gpio_set(CC2500_PA_GPIO, CC2500_PA_PIN);   // 1
    delay_us(5);
}


inline void cc2500_enable_receive(void) {
    // switch on rx again
    cc2500_enter_rxmode();
}


inline uint8_t cc2500_get_gdo_status(void) {
    if (gpio_get(CC2500_GDO1_GPIO, CC2500_GDO1_PIN)) {
        return 1;
    } else {
        return 0;
    }
}

inline void cc2500_read_fifo(uint8_t *buf, uint8_t len) {
    cc2500_register_read_multi(CC2500_FIFO | READ_FLAG | BURST_FLAG, buf, len);
}

inline void cc2500_register_read_multi(uint8_t address, uint8_t *buffer, uint8_t len) {
    // select device:
    cc2500_csn_lo();

    // wait for ready signal
    while (gpio_get(CC2500_SPI_GPIO, CC2500_SPI_MISO_PIN) == 1) {}

    // debug("read "); debug_put_uint8(len); debug_flush();
    // request address(read request)
    #if CC2500_DEBUG_STATUSBYTE
        uint8_t status = spi_tx(address);
        debug("spi status = ");
        debug_put_hex8(status);
        debug_put_newline();
    #else
        spi_tx(address);
    #endif  // CC2500_DEBUG_STATUSBYT

    // ill buffer with read commands:
    memset(buffer, 0xFF, len);

    spi_dma_xfer(buffer, len);
    /*
    while (len--) {
        *buf = spi_rx();
        buf++;
    }*/

    // deselect device
    cc2500_csn_hi();
}


inline void cc2500_register_write_multi(uint8_t address, uint8_t *buffer, uint8_t len) {
    // select device:
    cc2500_csn_lo();

    // wait for RDY signal:
    while (gpio_get(CC2500_SPI_GPIO, CC2500_SPI_MISO_PIN)) {}

    // request address(write request)
    spi_tx(address | BURST_FLAG);

    // send array
    spi_dma_xfer(buffer, len);

    // deselect device
    cc2500_csn_hi();
}

inline void cc2500_process_packet(volatile uint8_t *packet_received, volatile uint8_t *buffer, \
                                  uint8_t maxlen) {
    if (cc2500_get_gdo_status() == 1) {
        // data received, fetch data
        // timeout_set_100us(5);

        *packet_received = 0;

        // there is a bug in the cc2500
        // see p3 http:// www.ti.com/lit/er/swrz002e/swrz002e.pdf
        // workaround: read len register very quickly twice:
        uint8_t len1, len2, len, i;

        // try this 10 times befor giving up:
        for (i = 0; i < 10; i++) {
            len1 = cc2500_get_register_burst(RXBYTES) & 0x7F;
            len2 = cc2500_get_register_burst(RXBYTES) & 0x7F;
            if (len1 == len2) break;
        }

        // valid len found?
        if (len1 == len2) {
            len = len1;

            // packet received, grab data
            uint8_t tmp_buffer[len];
            cc2500_read_fifo(tmp_buffer, len);

            // only accept valid packet lengths:
            if (len == maxlen) {
                for (i = 0; i < maxlen; i++) {
                    buffer[i] = tmp_buffer[i];
                }
                *packet_received = 1;
            }
        } else {
            // no, ignore this
            len = 0;
        }
    }
}

void cc2500_transmit_packet(volatile uint8_t *buffer, uint8_t len) {
    // flush tx fifo
    cc2500_strobe(RFST_SFTX);
    // copy to fifo
    cc2500_register_write_multi(CC2500_FIFO, (uint8_t *)buffer, len);
    // and send!
    cc2500_strobe(RFST_STX);
}

/*
void cc2500_wait_for_transmission_complete(void) {
    // after STX we go back to RX state(see MCSM1 register)
    // so wait a maximum of 9ms for completion
    timeout2_set_100us(90);

    while (!timeout2_timed_out()) {
        if (cc2500_transmission_completed()) {
            // done with tx, return
            return;
        }
        // delay_us(100*1000);
    }

    // if we reach this point, tx timed out:
    debug("!TX");
}*/
