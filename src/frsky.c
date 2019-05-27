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
    along with this program.  If not, see <http:// www.gnu.org/ licenses/>.

    author: fishpepper <AT> gmail.com
*/

#include "main.h"
#include <string.h>
#include <stdio.h>
#include "frsky.h"
#include "debug.h"
#include "timeout.h"
#include "led.h"
#include "delay.h"
#include "wdt.h"
#include "cc2500.h"
#include "io.h"
#include "clocksource.h"
#include "storage.h"
#include "adc.h"
#include "telemetry.h"

#include <libopencm3/stm32/timer.h>

// this will make binding not very reliable, use for debugging only!
#define FRSKY_DEBUG_BIND_DATA 0
#define FRSKY_DEBUG_HOPTABLE 1

// DONE when n times a one:
#define MAX_BIND_PACKET_COUNT 10
#define HOPDATA_RECEIVE_DONE ((1  <<  (MAX_BIND_PACKET_COUNT))-1)


static volatile uint8_t frsky_frame_counter;
static uint8_t frsky_last_requested_telemetry_id;
static volatile uint32_t frsky_state;

// hop data & config
// uint8_t storage.frsky_txid[2] = {0x16, 0x68};
// uint8_t storage.frsky_hop_table[FRSKY_HOPTABLE_SIZE] = {0x01, 0x42, 0x83, 0xC4, 0x1A,
// 0x5B, 0x9C, 0xDD, 0x33, 0x74, 0xB5, 0x0B, 0x4C, 0x8D, 0xCE, 0x24, 0x65, 0xA6, 0xE7,
// 0x3D, 0x7E, 0xBF, 0x15, 0x56, 0x97, 0xD8, 0x2E, 0x6F, 0xB0, 0x06, 0x47, 0x88, 0xC9,
// 0x1F, 0x60, 0xA1, 0xE2, 0x38, 0x79, 0xBA, 0x10, 0x51, 0x92, 0xD3, 0x29, 0x6A, 0xAB};
// int8_t storage.frsky_freq_offset;
static uint8_t frsky_current_ch_idx;

// iversity counter
static uint8_t frsky_diversity_count;

// rssi
static uint8_t frsky_rssi;
static uint8_t frsky_rssi_telemetry;
static uint8_t frsky_link_quality;

// pll calibration
static uint8_t frsky_calib_fscal1_table[FRSKY_HOPTABLE_SIZE];
static uint8_t frsky_calib_fscal2;
static uint8_t frsky_calib_fscal3;
// int16_t storage.frsky_freq_offset_acc;

static uint8_t frsky_tx_enabled;

// rf rxtx buffer
static uint8_t frsky_bind_packet_received;
static uint16_t frsky_bind_packet_hoptable_flags;
static volatile uint8_t frsky_packet_buffer[FRSKY_PACKET_BUFFER_SIZE];
static volatile uint8_t frsky_packet_received;
static volatile uint8_t frsky_packet_sent;


void frsky_init(void) {
    // uint8_t i;
    debug("frsky: init\n"); debug_flush();

    telemetry_init();

    cc2500_init();

    frsky_link_quality = 0;
    frsky_diversity_count = 0;
    frsky_packet_received = 0;
    frsky_packet_sent = 0;
    frsky_bind_packet_received = 0;

    frsky_rssi = 100;

    // check if spi is working properly
    if (!frsky_check_transceiver()) {
        // no cc2500 detected - abort
        debug("frsky: no cc2500 detected. abort\n");
        debug_flush();
        return;
    }

    // init frsky registersttings for cc2500
    frsky_configure();

    // show info:
    debug("frsky: using txid 0x"); debug_flush();
    debug_put_hex8(storage.frsky_txid[0]);
    debug_put_hex8(storage.frsky_txid[1]);
    debug_put_newline();

    // init txid matching
    frsky_configure_address();

    // tune cc2500 pll and save the values to ram
    frsky_calib_pll();

    // initialise 9ms timer isr
    frsky_tx_enabled = 0;
    frsky_init_timer();

    frsky_tx_set_enabled(1);

    debug("frsky: init done\n"); debug_flush();
}

void frsky_init_timer(void) {
    // TIM3 clock enable
    rcc_periph_clock_enable(RCC_TIM3);

    // init timer3 for 9ms
    timer_reset(TIM3);

    // enable the TIM3 gloabal Interrupt
    nvic_enable_irq(NVIC_TIM3_IRQ);
    nvic_set_priority(NVIC_TIM3_IRQ, NVIC_PRIO_FRSKY);

    // compute prescaler value
    // we want one ISR every 9ms
    // setting TIM_Period to 9000 will reuqire
    // a prescaler so that one timer tick is 1us (1MHz)
    uint16_t prescaler = (uint16_t) (rcc_timer_frequency  / 1000000) - 1;

    // time base as calculated above
    timer_set_prescaler(TIM3, prescaler);
    timer_set_mode(TIM3, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
    // timer should count with 1MHz thus 9000 ticks = 9ms
    timer_set_period(TIM3, 9000-1);

    // DO NOT ENABLE INT yet!

    // enable timer
    timer_enable_counter(TIM3);
}

void frsky_tx_set_enabled(uint32_t enabled) {
    // TIM Interrupts enable? -> tx active
    if (enabled) {
        frsky_frame_counter = 0;
        frsky_state = 0;
        // enable ISR
        timer_enable_irq(TIM3, TIM_DIER_UIE);
    } else {
        // stop ISR
        timer_disable_irq(TIM3, TIM_DIER_UIE);
        // make sure last packet was sent
        delay_ms(20);
    }
}

static void frsky_send_packet(void) {
    // Stop RX DMA
    cc2500_strobe(RFST_SFRX);

    // enable tx
    cc2500_enter_txmode();

    // fetch adc channel data
    adc_process();
    uint16_t adc_data[8];
    uint32_t i;
    for (i = 0; i < 8; i++) {
        adc_data[i] = adc_get_channel_packetdata(i);
    }

    // build frsky packet
    // packet length
    frsky_packet_buffer[0] = 0x11;
    // txid
    frsky_packet_buffer[1] = storage.frsky_txid[0];
    frsky_packet_buffer[2] = storage.frsky_txid[1];
    // frame counter
    frsky_packet_buffer[3] = frsky_frame_counter;
    // last received telemetry frame
    frsky_packet_buffer[4] = frsky_last_requested_telemetry_id;
    // my tgy 9x sends 0x0B (is this how many bytes are free in hub buf?)
    frsky_packet_buffer[5] = 0x0B;
    // 6 .. 9 is LO(channel data 0..3)
    frsky_packet_buffer[6] = adc_data[0] & 0xFF;
    frsky_packet_buffer[7] = adc_data[1] & 0xFF;
    frsky_packet_buffer[8] = adc_data[2] & 0xFF;
    frsky_packet_buffer[9] = adc_data[3] & 0xFF;
    // 10 .. 11 is HI(channel data 0..3)
    frsky_packet_buffer[10] = ((adc_data[0]>>8) & 0x0F) | ((adc_data[1]>>4) & 0xF0);
    frsky_packet_buffer[11] = ((adc_data[2]>>8) & 0x0F) | ((adc_data[3]>>4) & 0xF0);
    // 12 .. 15 is LO(channel data 4..7)
    frsky_packet_buffer[12] = adc_data[4] & 0xFF;
    frsky_packet_buffer[13] = adc_data[5] & 0xFF;
    frsky_packet_buffer[14] = adc_data[6] & 0xFF;
    frsky_packet_buffer[15] = adc_data[7] & 0xFF;
    // 16 .. 17 is HI(channel data 4..7)
    frsky_packet_buffer[16] = ((adc_data[4]>>8) & 0x0F) | ((adc_data[5]>>4) & 0xF0);
    frsky_packet_buffer[17] = ((adc_data[6]>>8) & 0x0F) | ((adc_data[7]>>4) & 0xF0);

    // send packet
    cc2500_transmit_packet(frsky_packet_buffer, frsky_packet_buffer[0] + 1);
}

static uint8_t frsky_packet_lost_counter;

static void frsky_receive_packet(void) {
    // fetch incoming packet
    cc2500_process_packet(&frsky_packet_received, (volatile uint8_t *)&frsky_packet_buffer, \
                      FRSKY_PACKET_BUFFER_SIZE);

    // increment counter, will be cleared on valid packet rx
    frsky_packet_lost_counter++;

    // packet received?
    if (frsky_packet_received) {
        // decrypt data
        if (FRSKY_VALID_PACKET(frsky_packet_buffer)) {
            // reset lost packet counter
            frsky_packet_lost_counter = 0;

            // extract RSSI
            frsky_rssi = frsky_rssi + (8 * ((uint32_t)frsky_packet_buffer[5] -
                                       (uint32_t)frsky_rssi)) / 128;
            // frsky_rssi           = (((uint32_t)frsky_packet_buffer[5])*10 - 310)*12987/10000;
            frsky_rssi_telemetry = frsky_rssi_telemetry + (8 * (
                                        (uint32_t)frsky_extract_rssi(frsky_packet_buffer[18]) -
                                        (uint32_t)frsky_rssi_telemetry)) / 128;

            // extract telemetry packets:
            // buffer[0]  = bytes used
            // buffer[1]  = last received telemetry id
            // buffer[2]  = byte 1
            // ...
            // buffer[11] = byte 10
            //
            // NOTES:
            // * buffer[ 0] corresponds to frsky_buffer[ 6]
            // * buffer[11] corresponds to frsky_buffer[17]
            // * not all 10 bytes has to be filled in, only the number of bytes that were received
            //   will be sent in one frame
            //
            uint8_t telemetry_frame_id = frsky_packet_buffer[7];
            if (telemetry_frame_id == frsky_last_requested_telemetry_id) {
                // request new data with next packet
                frsky_last_requested_telemetry_id = (frsky_last_requested_telemetry_id + 1) & 0x1F;

                // extract data
                uint8_t bytecount = min(frsky_packet_buffer[6], 10);
                uint8_t i;
                for (i = 0; i < bytecount; i++) {
                    telemetry_enqueue(frsky_packet_buffer[8 + i]);
                }
            }

            /*debug_flush();
            uint32_t i;
            for (i = 0; i < FRSKY_PACKET_BUFFER_SIZE; i++) {
                debug_put_hex8(frsky_packet_buffer[i]);
                debug_putc(' ');
            }
            debug_put_newline();*/
        }
    }



    // handle any ovf conditions
    frsky_handle_overflows();
}

void frsky_handle_telemetry(void) {
    // handle incoming telemetry data
    telemetry_process();
}


void frsky_get_rssi(uint8_t *rssi, uint8_t *rssi_telemetry) {
    if (frsky_packet_lost_counter > 20) {
        *rssi           = 0;
        *rssi_telemetry = 0;
    } else {
        *rssi           = frsky_rssi;
        *rssi_telemetry = frsky_rssi_telemetry;
    }
}

void TIM3_IRQHandler(void) {
    if (timer_get_flag(TIM3, TIM_SR_UIF)) {
        // clear flag (NOTE: this should never be done at the end of the ISR)
        timer_clear_flag(TIM3, TIM_SR_UIF);

        // when will there be the next isr?
        switch (frsky_state) {
            default:
            case (0) :
                // process any incoming telemetry data
                frsky_receive_packet();
                // hop to next channel
                frsky_increment_channel(1);
                // send data
                frsky_send_packet();
                frsky_frame_counter++;
                // next hop in 9ms
                timer_set_period(TIM3, 9000-1);
                frsky_state = 1;
                break;

            case (1) :
                // hop to next channel
                frsky_increment_channel(1);
                // send data
                frsky_send_packet();
                frsky_frame_counter++;
                // next hop in 9ms
                timer_set_period(TIM3, 9000-1);
                frsky_state = 2;
                break;

            case (2) :
                // hop to next channel
                frsky_increment_channel(1);
                // send data
                frsky_send_packet();
                frsky_frame_counter++;
                // after this tx we expect rx data,
                // TX is finished after ~7.2ms -> next isr in 7.5ms
                timer_set_period(TIM3, 7500-1);
                frsky_state = 3;
                break;

            case (3) :
                // prepare for data receiption, hop to next channel
                frsky_increment_channel(1);
                // enable LNA
                cc2500_enter_rxmode();
                // the next hop will now in 1.3ms (after freq stabilised)
                timer_set_period(TIM3, 1300-1-0*200);
                frsky_state = 4;
                break;

            case (4) :
                // now go to RX mode
                // go to idle
                cc2500_strobe(RFST_SRX);
                // increment framecounter
                frsky_frame_counter++;
                // the next hop will now in 9.2ms
                // 2*9 = 18 = 7.5 + 1.3 + 9.2
                timer_set_period(TIM3, 9200-1+0*200);
                frsky_state = 0;
                break;

            case (0x80) :
                // bind mode, set timeout to 9ms
                timer_set_period(TIM3, 9000);

                // get bind packet index
                frsky_frame_counter++;
                if ((frsky_frame_counter * 5) > FRSKY_HOPTABLE_SIZE) {
                    frsky_frame_counter = 0;
                }

                // send bind packet
                frsky_send_bindpacket(frsky_frame_counter);
                frsky_state = 0x80;
                break;
        }
    }
}


void frsky_send_bindpacket(uint8_t bind_packet_id) {
    uint8_t i;

    // Stop RX DMA
    cc2500_strobe(RFST_SFRX);

    // frequency offset to zero(will do auto tune later on)
    storage.frsky_freq_offset = 0;

    // init txid matching
    frsky_configure_address();

    // set channel 0
    frsky_tune_channel(0);

    // enable tx
    cc2500_enter_txmode();

    // length of byte(always 0x11 = 17 bytes)
    frsky_packet_buffer[0] = 0x11;
    // bind identifier?
    frsky_packet_buffer[1] = 0x03;
    frsky_packet_buffer[2] = 0x01;
    // txid
    frsky_packet_buffer[3] = storage.frsky_txid[0];
    frsky_packet_buffer[4] = storage.frsky_txid[1];
    // hoptable index
    frsky_packet_buffer[5] = bind_packet_id * 5;

    // add a maximum of 5 bytes hoptable data
    for (i = 0; i < 5; i++) {
        uint8_t index = bind_packet_id * 5 + i;
        if (index < FRSKY_HOPTABLE_SIZE) {
            frsky_packet_buffer[6 + i] = storage.frsky_hop_table[index];
        } else {
            frsky_packet_buffer[6 + i] = 0;
        }
    }

    // fill with zeros
    for (i = 11; i < 17; i++) {
        frsky_packet_buffer[i] = 0;
    }

    debug("frsky: BIND");
    debug_put_hex8(frsky_packet_buffer[5]);
    debug_putc(' ');
    for (i=0; i < 5; i++) {
        debug_put_hex8(frsky_packet_buffer[6 + i]);
        debug_putc(' ');
    }
    debug("\n"); debug_flush();

    // send packet
    cc2500_transmit_packet(frsky_packet_buffer, frsky_packet_buffer[0] + 1);
}


uint8_t frsky_check_transceiver(void) {
    debug("frsky: partinfo\n"); debug_flush();

    uint8_t partnum, version;
    // start idle
    cc2500_strobe(RFST_SIDLE);

    // check version:
    debug("frsky: cc2500 partnum 0x");
    partnum = cc2500_get_register_burst(PARTNUM);
    debug_put_hex8(partnum);

    debug(" version 0x");
    version = cc2500_get_register_burst(VERSION);
    debug_put_hex8(version);
    debug_put_newline();

    if (cc2500_partnum_valid(partnum, version)) {
        debug("frsky: got valid part and version info\n");
        debug_flush();
        return 1;
    }

    // else
    debug("frsky: got INVALID part and version info?!\n");
    debug_flush();
    return 0;
}

void frsky_configure(void) {
    debug("frsky: configure\n"); debug_flush();

    // start idle
    cc2500_strobe(RFST_SIDLE);

    // IOCFG0,1,2 is set in hal code(it is specific to the board used)
    cc2500_set_gdo_mode();

    // normal config
    cc2500_set_register(MCSM1    , 0x0F);  // go back to rx after transmission completed
    cc2500_set_register(MCSM0    , 0x18);
    cc2500_set_register(PKTLEN   , FRSKY_PACKET_LENGTH);  // on 251x this has to be exactly our size
    cc2500_set_register(PKTCTRL0 , 0x05);
    cc2500_set_register(PA_TABLE0, 0xFF);
    cc2500_set_register(FSCTRL1  , 0x08);  // D4R-II seems to set 0x68 here ?! instead of 0x08
    cc2500_set_register(FSCTRL0  , 0x00);
    // set base freq 2404 mhz
    cc2500_set_register(FREQ2    , 0x5C);
    cc2500_set_register(FREQ1    , 0x76);
    cc2500_set_register(FREQ0    , 0x27);
    cc2500_set_register(MDMCFG4  , 0xAA);
    cc2500_set_register(MDMCFG3  , 0x39);
    cc2500_set_register(MDMCFG2  , 0x11);
    cc2500_set_register(MDMCFG1  , 0x23);
    cc2500_set_register(MDMCFG0  , 0x7A);
    cc2500_set_register(DEVIATN  , 0x42);
    cc2500_set_register(FOCCFG   , 0x16);
    cc2500_set_register(BSCFG    , 0x6C);
    cc2500_set_register(AGCCTRL2 , 0x03);
    cc2500_set_register(AGCCTRL1 , 0x40);  // D4R uses 46 instead of 0x40);
    cc2500_set_register(AGCCTRL0 , 0x91);
    cc2500_set_register(FREND1   , 0x56);
    cc2500_set_register(FREND0   , 0x10);
    cc2500_set_register(FSCAL3   , 0xA9);
    cc2500_set_register(FSCAL2   , 0x05);
    cc2500_set_register(FSCAL1   , 0x00);
    cc2500_set_register(FSCAL0   , 0x11);
    // ???FSTEST   , 0x59);
    cc2500_set_register(TEST2    , 0x88);
    cc2500_set_register(TEST1    , 0x31);
    cc2500_set_register(TEST0    , 0x0B);
    // cc2500_set_register(FIFOTHR  , 0x0F);  // fifo threshold -> tx gdo goes high if 1 byte left
    cc2500_set_register(ADDR     , 0x00);

    // for now just append status
    cc2500_set_register(PKTCTRL1, CC2500_PKTCTRL1_APPEND_STATUS);
    debug("frsky: configure done\n"); debug_flush();
}

uint8_t frsky_bind_jumper_set(void) {
    debug("frsky: BIND jumper set = "); debug_flush();
    if (0) {  // io_bind_request()) {
        debug("YES -> binding\n");
        return 1;
    } else {
        debug("NO -> no binding\n");
        return 0;
    }
}

void frsky_do_clone_prepare(void) {
    debug("frsky: do clone\n"); debug_flush();

    // set txid to bind channel
    storage.frsky_txid[0] = 0x03;

    // frequency offset to zero(will do auto tune later on)
    storage.frsky_freq_offset = 0;

    // init txid matching
    frsky_configure_address();

    // set up leds:
    led_button_r_off();
    led_button_l_on();
}

void frsky_enter_bindmode(void) {
    debug("frsky: do bind\n"); debug_flush();

    frsky_state = 0x80;

    // set up leds:
    led_button_r_on();
    led_button_l_off();
}


void frsky_do_clone_finish(void) {
    // important: stop RF interrupts:
    cc2500_disable_rf_interrupt();

    // save to persistant storage:
    storage_save();

    // done, end up in fancy blink code
    debug("frsky: finished binding. please reset\n");
    led_button_l_on();
}

static int8_t frsky_fscal0_min;
static int8_t frsky_fscal0_max;

void frsky_autotune_prepare(void) {
    debug("frsky: autotune\n"); debug_flush();

    // enter RX mode
    frsky_enter_rxmode(0);

    // find best offset:
    storage.frsky_freq_offset = 0;

    debug("frsky: entering bind loop\n"); debug_flush();

    led_button_r_off();

    frsky_state = 0;
    frsky_fscal0_min = 127;
    frsky_fscal0_max = -127;
    frsky_bind_packet_received = 0;
}

uint32_t frsky_autotune_do(void) {
    // search for best fscal 0 match
    // reset wdt
    wdt_reset();

    // handle any ovf conditions
    frsky_handle_overflows();

    // search full range quickly using binary search
    switch (frsky_state) {
        default:
        case (0):
            // init left search:
            storage.frsky_freq_offset = -127;
            frsky_state = 1;
            break;

        case (1):
            // first search quickly through the full range:
            if (storage.frsky_freq_offset < 127-10) {
                storage.frsky_freq_offset += 9;
            } else {
                // done one search, did we receive anything?
                if (frsky_bind_packet_received) {
                    // finished, go to slow search
                    storage.frsky_freq_offset = frsky_fscal0_min - 9;
                    frsky_state = 2;
                } else {
                    // no success, lets try again
                    frsky_state = 0;
                }
            }
            break;

        case (2):
            if (storage.frsky_freq_offset < frsky_fscal0_max+9) {
                storage.frsky_freq_offset++;
            } else {
                // done!
                frsky_state = 5;
            }
            break;
    }

    // go to idle
    cc2500_strobe(RFST_SIDLE);

    // set freq offset
    cc2500_set_register(FSCTRL0, storage.frsky_freq_offset);

    led_button_r_off();

    // go back to RX:
    delay_ms(1);
    cc2500_strobe(RFST_SRX);

    // set timeout
    timeout_set(50);

    led_button_l_on();
    led_button_r_off();

    // debug("tune "); debug_put_int8(storage.frsky_freq_offset);
    // debug_put_newline(); debug_flush();
    uint32_t done = 0;
    while ((!timeout_timed_out()) && (!done)) {
        frsky_packet_received = 0;

        // handle any ovf conditions
        frsky_handle_overflows();

        cc2500_process_packet(&frsky_packet_received, \
                              (volatile uint8_t *)&frsky_packet_buffer, \
                              FRSKY_PACKET_BUFFER_SIZE);

        if (frsky_packet_received) {
            // prepare for next packet:
            frsky_packet_received = 0;
            cc2500_enable_receive();
            cc2500_strobe(RFST_SRX);

            // valid packet?
            if (FRSKY_VALID_PACKET_BIND(frsky_packet_buffer)) {
                // bind packet!
                debug_putc('B');

                // packet received
                frsky_bind_packet_received = 1;

                // this fscal value is done
                done = 1;

                // update min/ max
                frsky_fscal0_min = min(frsky_fscal0_min, storage.frsky_freq_offset);
                frsky_fscal0_max = max(frsky_fscal0_max, storage.frsky_freq_offset);

                // make sure we never read the same packet twice by invalidating packet
                frsky_packet_buffer[0] = 0x00;
            }

            /*debug("[");debug_flush();
    uint8_t cnt;
            for (cnt = 0; cnt < FRSKY_PACKET_BUFFER_SIZE; cnt++) {
                debug_put_hex8(frsky_packet_buffer[cnt]);
                debug_putc(' ');
                debug_flush();
            }
            debug("]\n"); debug_flush();*/
        }
    }
    if (!done) {
        debug_putc('-');
    }
    debug_flush();

    return (frsky_state == 5);
}


void frsky_autotune_finish(void) {
    // set offset to what we found out to be the best:
    int16_t fscal0_calc = (frsky_fscal0_max + frsky_fscal0_min)/ 2;

    debug("\nfrsky: fscal0 ");
    debug_put_int8(frsky_fscal0_min);
    debug(" - ");
    debug_put_int8(frsky_fscal0_max);
    debug_put_newline();
    debug_flush();

    // store new value
    storage.frsky_freq_offset = fscal0_calc;

    cc2500_strobe(RFST_SIDLE);

    // set freq offset
    cc2500_set_register(FSCTRL0, storage.frsky_freq_offset);

    // go back to RX:
    delay_ms(1);
    cc2500_strobe(RFST_SRX);

    debug("frsky: autotune done\n");
    debug("frsky: offset=");
    debug_put_int8(storage.frsky_freq_offset);
    debug_put_newline();
    debug_flush();
}


void frsky_enter_rxmode(uint8_t channel) {
    cc2500_strobe(RFST_SIDLE);

    cc2500_enter_rxmode();

    // set & do a manual tuning for the given channel
    frsky_tune_channel(channel);

    cc2500_enable_receive();

    // go back to rx mode
    cc2500_strobe(RFST_SRX);
}


void frsky_configure_address(void) {
    // start idle
    cc2500_strobe(RFST_SIDLE);

    // freq offset
    cc2500_set_register(FSCTRL0, storage.frsky_freq_offset);

    // never automatically calibrate, po_timeout count = 64
    // no autotune as(we use our pll map)
    cc2500_set_register(MCSM0, 0x08);

    // set address
    cc2500_set_register(ADDR, storage.frsky_txid[0]);

    // append status, filter by address, autoflush on bad crc, PQT = 0
    cc2500_set_register(PKTCTRL1, CC2500_PKTCTRL1_APPEND_STATUS | CC2500_PKTCTRL1_CRC_AUTOFLUSH | \
                        CC2500_PKTCTRL1_FLAG_ADR_CHECK_01);
}


void frsky_tune_channel(uint8_t ch) {
    // start idle
    cc2500_strobe(RFST_SIDLE);

    // set channel number
    cc2500_set_register(CHANNR, ch);

    // start Self calib:
    cc2500_strobe(RFST_SCAL);

    // wait for scal end
    // either delay_us(800) or check MARCSTATE:
    while (cc2500_get_register(MARCSTATE) != 0x01) {}

    // now FSCAL3..1 shold be set up correctly! yay!
}

void frsky_handle_overflows(void) {
    uint8_t marc_state;

    // fetch marc status
    marc_state = cc2500_get_register(MARCSTATE) & 0x1F;
    if (marc_state == 0x11) {
        debug("frsky: RXOVF\n");
        // flush rx buf
        cc2500_strobe(RFST_SFRX);
        // cc2500_strobe(RFST_SIDLE);
    } else if (marc_state == 0x16) {
        debug("frsky: TXOVF\n");
        // flush tx buf
        cc2500_strobe(RFST_SFTX);
        // cc2500_strobe(RFST_SIDLE);
    }
}


void frsky_fetch_txid_and_hoptable_prepare(void) {
    debug("frsky: fetching txid + hopt\n"); debug_flush();

    // enter RX mode
    frsky_enter_rxmode(0);

    // clear txid:
    storage.frsky_txid[0] = 0;
    storage.frsky_txid[1] = 0;

    // timeout to wait for packets
    timeout_set(9*3+1);
    frsky_bind_packet_received = 0;
    frsky_bind_packet_hoptable_flags = 0;
}

uint32_t frsky_fetch_txid_and_hoptable_do(void) {
    uint8_t i;

    // fetch hopdata array
    // reset wdt
    wdt_reset();

    // handle any ovf conditions
    frsky_handle_overflows();

    // FIXME: this should be handled in a cleaner way.
    // as this is just for binding, stay with this fix for now...
    if (timeout_timed_out()) {
        // do diversity
        // cc2500_switch_antenna();

        debug_putc('m');

        // next packet should be there ein 9ms
        // if no packet for 3*9ms -> reset rx chain:
        timeout_set(3*9+1);

        // re-prepare for next packet:
        cc2500_strobe(RFST_SIDLE);
        // TESTME: moved to rx_sleep....
        // delay_ms(1);
        frsky_packet_received = 0;
        cc2500_rx_sleep();
        cc2500_enable_receive();
        cc2500_strobe(RFST_SRX);
    }

    // process incoming data
    cc2500_process_packet(&frsky_packet_received, (volatile uint8_t *)&frsky_packet_buffer, \
                          FRSKY_PACKET_BUFFER_SIZE);

    if (frsky_packet_received) {
        debug_putc('p');

        // prepare for next packet:
        frsky_packet_received = 0;
        cc2500_enable_receive();
        cc2500_strobe(RFST_SRX);


#if FRSKY_DEBUG_BIND_DATA
        if (FRSKY_VALID_FRAMELENGTH(frsky_packet_buffer)) {
            debug("frsky: RX ");
            debug_flush();
            for (i = 0; i < FRSKY_PACKET_BUFFER_SIZE; i++) {
                debug_put_hex8(frsky_packet_buffer[i]);
                debug_putc(' ');
            }
            debug_put_newline();
        }
#endif  // FRSKY_DEBUG_BIND_DATA


        // do we know our txid yet?
        if (FRSKY_VALID_PACKET_BIND(frsky_packet_buffer)) {
            // next packet should be ther ein 9ms
            // if no packet for 3*9ms -> reset rx chain:
            timeout_set(3*9+1);

            debug_putc('B');
            if ((storage.frsky_txid[0] == 0) && (storage.frsky_txid[1] == 0)) {
                // no! extract this
                storage.frsky_txid[0] = frsky_packet_buffer[3];
                storage.frsky_txid[1] = frsky_packet_buffer[4];
                // debug
                debug("\nfrsky: got txid 0x");
                debug_put_hex8(storage.frsky_txid[0]);
                debug_put_hex8(storage.frsky_txid[1]);
                debug_put_newline();
            }

            // this is actually for us
            uint8_t index = frsky_packet_buffer[5];

            // valid bind index?
            if (index/ 5 < MAX_BIND_PACKET_COUNT) {
                // copy data to our hop list:
                for (i = 0; i < 5; i++) {
                    if ((index+i) < FRSKY_HOPTABLE_SIZE) {
                        storage.frsky_hop_table[index+i] = frsky_packet_buffer[6+i];
                    }
                }
                // mark as done: set bit flag for index
                frsky_bind_packet_hoptable_flags |= (1 << (index/ 5));
            } else {
                debug("frsky: invalid bind idx");
                debug_put_uint8(index / 5);
                debug_put_newline();
            }

            // make sure we never read the same packet twice by crc flag
            frsky_packet_buffer[FRSKY_PACKET_BUFFER_SIZE-1] = 0x00;
        }
    }
    debug_put_uint8(frsky_bind_packet_hoptable_flags);
    debug_put_newline();

    debug_flush();

    return (frsky_bind_packet_hoptable_flags == HOPDATA_RECEIVE_DONE);
}

void frsky_fetch_txid_and_hoptable_finish(void) {
#if FRSKY_DEBUG_BIND_DATA
    debug("frsky: hop[] = ");
    for (i = 0; i < FRSKY_HOPTABLE_SIZE; i++) {
        debug_put_hex8(storage.frsky_hop_table[i]);
        debug_putc(' ');
        debug_flush();
    }
#endif  // FRSKY_DEBUG_BIND_DATA
    debug_putc('\n');
    debug_flush();

    // idle
    cc2500_strobe(RFST_SIDLE);
}

void frsky_calib_pll(void) {
    uint8_t i;
    uint8_t ch;

    debug("frsky: calib pll\n");

    // fine tune offset
    cc2500_set_register(FSCTRL0, storage.frsky_freq_offset);

    debug("frsky: tuning hop[] =");

    // calibrate pll for all channels
    for (i = 0; i < FRSKY_HOPTABLE_SIZE; i++) {
        // reset wdt
        wdt_reset();

        // fetch channel from hop_table:
        ch = storage.frsky_hop_table[i];

        // debug info
        if (i < 9) {
            debug_put_hex8(ch);
            debug_putc(' ');
        }

        // set channel number
        frsky_tune_channel(ch);

        // store pll calibration:
        frsky_calib_fscal1_table[i] = cc2500_get_register(FSCAL1);
    }
    debug_put_newline();

    // only needed once:
    frsky_calib_fscal3 = cc2500_get_register(FSCAL3);
    frsky_calib_fscal2 = cc2500_get_register(FSCAL2);

    // return to idle
    cc2500_strobe(RFST_SIDLE);

    debug("...\nfrsky: calib fscal0 = ");
    debug_put_int8(storage.frsky_freq_offset);
    debug("\nfrsky: calib fscal1:\n");
    for (i = 0; i < 9; i++) {
        debug_put_hex8(frsky_calib_fscal1_table[i]);
        debug_putc(' ');
    }
    debug("...\nfrsky: calib fscal2 = 0x");
    debug_put_hex8(frsky_calib_fscal2);
    debug("\nfrsky: calib fscal3 = 0x");
    debug_put_hex8(frsky_calib_fscal3);
    debug_put_newline();
    debug_flush();

    debug("frsky: calib pll done\n");
}

void frsky_set_channel(uint8_t hop_index) {
    uint8_t ch = storage.frsky_hop_table[hop_index];
    // debug_putc('S'); debug_put_hex8(ch);

    // go to idle
    cc2500_strobe(RFST_SIDLE);

    // fetch and set our stored pll calib data:
    cc2500_set_register(FSCAL3, frsky_calib_fscal3);
    cc2500_set_register(FSCAL2, frsky_calib_fscal2);
    cc2500_set_register(FSCAL1, frsky_calib_fscal1_table[hop_index]);

    // set channel
    cc2500_set_register(CHANNR, ch);
}



void frsky_increment_channel(int8_t cnt) {
    int8_t next = frsky_current_ch_idx;
    // add increment
    next+=cnt;
    // convert to a safe unsigned number:
    if (next < 0) {
        next += FRSKY_HOPTABLE_SIZE;
    }
    if (next >= FRSKY_HOPTABLE_SIZE) {
        next -= FRSKY_HOPTABLE_SIZE;
    }

    frsky_current_ch_idx = next;
    frsky_set_channel(frsky_current_ch_idx);
}


uint8_t frsky_extract_rssi(uint8_t rssi_raw) {
#define FRSKY_RSSI_OFFSET 70
    if (rssi_raw >= 128) {
        // adapted to fit better to the original values... FIXME: find real formula
        // return(rssi_raw * 18)/ 32 - 82;
        return((((uint16_t)rssi_raw) * 18)>>5) - 82;
    } else {
        return((((uint16_t)rssi_raw) * 18)>>5) + 65;
    }
}


/*
void frsky_send_telemetry(uint8_t telemetry_id) {
    static uint8_t test = 0;

    // Stop RX DMA
    cc2500_strobe(RFST_SFRX);

    // enable tx
    cc2500_enter_txmode();

    // length of byte(always 0x11 = 17 bytes)
    frsky_packet_buffer[0] = 0x11;
    // txid
    frsky_packet_buffer[1] = storage.frsky_txid[0];
    frsky_packet_buffer[2] = storage.frsky_txid[1];
    // ADC channels
    frsky_packet_buffer[3] = 123;  // adc_get_scaled(0);
    frsky_packet_buffer[4] = 123;  // adc_get_scaled(1);
    // RSSI
    frsky_packet_buffer[5] = 123;  // frsky_rssi;

    {
        uint8_t i;
        for (i = 6; i < FRSKY_PACKET_BUFFER_SIZE; i++) {
            frsky_packet_buffer[i] = 0;
        }
    }

    // append any received hub telemetry data
    /// telemetry_fill_buffer(&frsky_packet_buffer[6], telemetry_id);

    // re arm adc dma etc
    // it is important to call this after reading the values...
    adc_process();

    // send packet
    cc2500_transmit_packet(frsky_packet_buffer, FRSKY_PACKET_BUFFER_SIZE);

    // wait for transmission complete:
    cc2500_wait_for_transmission_complete();

    // prepare for rx:
    cc2500_setup_rf_dma(cc2500_MODE_RX);
    cc2500_enable_receive();
    // SHOULD NOT BE NECESSARY, DEFAULT IS BACK TO RX// cc2500_strobe(RFST_SRX);
}
*/

#if 0




uint8_t frsky_append_hub_data(uint8_t sensor_id, uint16_t value, uint8_t *buf) {
    uint8_t index = 0;
    uint8_t val8;

    // add header:
    buf[index++] = FRSKY_HUB_TELEMETRY_HEADER;
    // add sensor id
    buf[index++] = sensor_id;
    // add data, take care of byte stuffing
    // low byte
    val8 = LO(value);
    if (val8 == 0x5E) {
        buf[index++] = 0x5D;
        buf[index++] = 0x3E;
    } else if (val8 == 0x5D) {
        buf[index++] = 0x5D;
        buf[index++] = 0x3D;
    } else {
        buf[index++] = val8;
    }
    // high byte
    val8 = HI(value);
    if (val8 == 0x5E) {
        buf[index++] = 0x5D;
        buf[index++] = 0x3E;
    } else if (val8 == 0x5D) {
        buf[index++] = 0x5D;
        buf[index++] = 0x3D;
    } else {
        buf[index++] = val8;
    }
    // add footer:
    buf[index] = FRSKY_HUB_TELEMETRY_HEADER;

    // return index:
    return index;
}
#endif  // 0
