//-----------------------------------------------------------------------------
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Hitag2 emulation
//
// (c) 2009 Henryk Plötz <henryk@ploetzli.ch>
//-----------------------------------------------------------------------------
// Hitag2 complete rewrite of the code
// - Fixed modulation/encoding issues
// - Rewrote code for transponder emulation
// - Added sniffing of transponder communication
// - Added reader functionality
//
// (c) 2012 Roel Verdult
//-----------------------------------------------------------------------------
// Piwi, 2019
// Iceman, 2019
// Anon, 2019

#include "hitag2.h"
#include "hitag2_crypto.h"
#include "string.h"
#include "proxmark3_arm.h"
#include "cmd.h"
#include "BigBuf.h"
#include "fpgaloader.h"
#include "ticks.h"
#include "dbprint.h"
#include "util.h"
#include "lfadc.h"
#include "lfsampling.h"
#include "lfdemod.h"
#include "commonutil.h"

// Successful crypto auth
static bool bCrypto;
// Is in auth stage
static bool bAuthenticating;
// Successful password auth
bool bSelecting;
bool bCollision;
static bool bPwd;
static bool bSuccessful;

static struct hitag2_tag tag = {
    .state = TAG_STATE_RESET,
    .sectors = {                         // Password mode:               | Crypto mode:
        [0]  = { 0x02, 0x4e, 0x02, 0x20}, // UID                          | UID
        [1]  = { 0x4d, 0x49, 0x4b, 0x52}, // Password RWD                 | 32 bit LSB key
        [2]  = { 0x20, 0xf0, 0x4f, 0x4e}, // Reserved                     | 16 bit MSB key, 16 bit reserved
        [3]  = { 0x0e, 0xaa, 0x48, 0x54}, // Configuration, password TAG  | Configuration, password TAG
        [4]  = { 0x46, 0x5f, 0x4f, 0x4b}, // Data: F_OK
        [5]  = { 0x55, 0x55, 0x55, 0x55}, // Data: UUUU
        [6]  = { 0xaa, 0xaa, 0xaa, 0xaa}, // Data: ....
        [7]  = { 0x55, 0x55, 0x55, 0x55}, // Data: UUUU
        [8]  = { 0x00, 0x00, 0x00, 0x00}, // RSK Low
        [9]  = { 0x00, 0x00, 0x00, 0x00}, // RSK High
        [10] = { 0x00, 0x00, 0x00, 0x00}, // RCF
        [11] = { 0x00, 0x00, 0x00, 0x00}, // SYNC
        // up to index 15 reserved for HITAG1/HITAGS public data
    },
};

static enum {
    WRITE_STATE_START = 0x0,
    WRITE_STATE_PAGENUM_WRITTEN,
    WRITE_STATE_PROG
} writestate;


// ToDo: define a meaningful maximum size for auth_table. The bigger this is, the lower will be the available memory for traces.
// Historically it used to be FREE_BUFFER_SIZE, which was 2744.
#define AUTH_TABLE_LENGTH 2744
static uint8_t *auth_table;
static size_t auth_table_pos = 0;
static size_t auth_table_len = AUTH_TABLE_LENGTH;

static uint8_t password[4];
static uint8_t NrAr[8];
static uint8_t key[8];
static uint8_t writedata[4];
uint8_t logdata_0[4], logdata_1[4];
uint8_t nonce[4];
bool key_no;
static uint64_t cipher_state;

static int hitag2_reset(void) {
    tag.state = TAG_STATE_RESET;
    tag.crypto_active = 0;
    return 0;
}

static int hitag2_init(void) {
    hitag2_reset();
    return 0;
}

// Sam7s has several timers, we will use the source TIMER_CLOCK1 (aka AT91C_TC_CLKS_TIMER_DIV1_CLOCK)
// TIMER_CLOCK1 = MCK/2, MCK is running at 48 MHz, Timer is running at 48/2 = 24 MHz
// Hitag units (T0) have duration of 8 microseconds (us), which is 1/125000 per second (carrier)
// T0 = TIMER_CLOCK1 / 125000 = 192
#ifndef T0
#define T0               192
#endif

#define HITAG_FRAME_LEN  20
#define HITAG_T_STOP     36 /* T_EOF should be > 36 */
#define HITAG_T_LOW      8  /* T_LOW should be 4..10 */
#define HITAG_T_0_MIN    15 /* T[0] should be 18..22 */
#define HITAG_T_1_MIN    25 /* T[1] should be 26..30 */
//#define HITAG_T_EOF      40 /* T_EOF should be > 36 */
#define HITAG_T_EOF      80 /* T_EOF should be > 36 */
#define HITAG_T_WAIT_1   200 /* T_wresp should be 199..206 */
#define HITAG_T_WAIT_2   90 /* T_wresp should be 199..206 */
#define HITAG_T_WAIT_MAX 300 /* bit more than HITAG_T_WAIT_1 + HITAG_T_WAIT_2 */
#define HITAG_T_PROG     614

#define HITAG_T_TAG_ONE_HALF_PERIOD     10
#define HITAG_T_TAG_TWO_HALF_PERIOD     25
#define HITAG_T_TAG_THREE_HALF_PERIOD   41
#define HITAG_T_TAG_FOUR_HALF_PERIOD    57

#define HITAG_T_TAG_HALF_PERIOD         16
#define HITAG_T_TAG_FULL_PERIOD         32

#define HITAG_T_TAG_CAPTURE_ONE_HALF    13
#define HITAG_T_TAG_CAPTURE_TWO_HALF    25
#define HITAG_T_TAG_CAPTURE_THREE_HALF  41
#define HITAG_T_TAG_CAPTURE_FOUR_HALF   57

static void hitag_send_bit(int bit) {
    LED_A_ON();
    // Reset clock for the next bit
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_SWTRG;

    // Fixed modulation, earlier proxmark version used inverted signal
    if (bit == 0) {
        // Manchester: Unloaded, then loaded |__--|
        LOW(GPIO_SSC_DOUT);
        while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_TAG_HALF_PERIOD);
        HIGH(GPIO_SSC_DOUT);
        while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_TAG_FULL_PERIOD);
    } else {
        // Manchester: Loaded, then unloaded |--__|
        HIGH(GPIO_SSC_DOUT);
        while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_TAG_HALF_PERIOD);
        LOW(GPIO_SSC_DOUT);
        while (AT91C_BASE_TC0->TC_CV < T0 * HITAG_T_TAG_FULL_PERIOD);
    }
    LED_A_OFF();
}

static void hitag_send_frame(const uint8_t *frame, size_t frame_len) {
    // SOF - send start of frame
    hitag_send_bit(1);
    hitag_send_bit(1);
    hitag_send_bit(1);
    hitag_send_bit(1);
    hitag_send_bit(1);

    // Send the content of the frame
    for (size_t i = 0; i < frame_len; i++) {
        hitag_send_bit((frame[i / 8] >> (7 - (i % 8))) & 1);
    }

    // Drop the modulation
    LOW(GPIO_SSC_DOUT);
}

// sim
static void hitag2_handle_reader_command(uint8_t *rx, const size_t rxlen, uint8_t *tx, size_t *txlen) {
    uint8_t rx_air[HITAG_FRAME_LEN];

    // Copy the (original) received frame how it is send over the air
    memcpy(rx_air, rx, nbytes(rxlen));

    if (tag.crypto_active) {
        hitag2_cipher_transcrypt(&(tag.cs), rx, rxlen / 8, rxlen % 8);
    }

    // Reset the transmission frame length
    *txlen = 0;

    // Try to find out which command was send by selecting on length (in bits)
    switch (rxlen) {
        // Received 11000 from the reader, request for UID, send UID
        case 05: {
            // Always send over the air in the clear plaintext mode
            if (rx_air[0] != 0xC0) {
                // Unknown frame ?
                return;
            }
            *txlen = 32;
            memcpy(tx, tag.sectors[0], 4);
            tag.crypto_active = 0;
        }
        break;

        // Read/Write command: ..xx x..y  yy with yyy == ~xxx, xxx is sector number
        case 10: {
            unsigned int sector = (~(((rx[0] << 2) & 0x04) | ((rx[1] >> 6) & 0x03)) & 0x07);
            // Verify complement of sector index
            if (sector != ((rx[0] >> 3) & 0x07)) {
                //DbpString("Transmission error (read/write)");
                return;
            }

            switch (rx[0] & 0xC6) {
                // Read command: 11xx x00y
                case 0xC0:
                    memcpy(tx, tag.sectors[sector], 4);
                    *txlen = 32;
                    break;

                // Inverted Read command: 01xx x10y
                case 0x44:
                    for (size_t i = 0; i < 4; i++) {
                        tx[i] = tag.sectors[sector][i] ^ 0xff;
                    }
                    *txlen = 32;
                    break;

                // Write command: 10xx x01y
                case 0x82:
                    // Prepare write, acknowledge by repeating command
                    memcpy(tx, rx, nbytes(rxlen));
                    *txlen = rxlen;
                    tag.active_sector = sector;
                    tag.state = TAG_STATE_WRITING;
                    break;

                // Unknown command
                default:
                    Dbprintf("Unknown command: %02x %02x", rx[0], rx[1]);
                    return;
                    break;
            }
        }
        break;

        // Writing data or Reader password
        case 32: {
            if (tag.state == TAG_STATE_WRITING) {
                // These are the sector contents to be written. We don't have to do anything else.
                memcpy(tag.sectors[tag.active_sector], rx, nbytes(rxlen));
                tag.state = TAG_STATE_RESET;
                return;
            } else {
                // Received RWD password, respond with configuration and our password
                if (memcmp(rx, tag.sectors[1], 4) != 0) {
                    DbpString("Reader password is wrong");
                    return;
                }
                *txlen = 32;
                memcpy(tx, tag.sectors[3], 4);
            }
        }
        break;

        // Received RWD authentication challenge and respnse
        case 64: {
            // Store the authentication attempt
            if (auth_table_len < (AUTH_TABLE_LENGTH - 8)) {
                memcpy(auth_table + auth_table_len, rx, 8);
                auth_table_len += 8;
            }

            // Reset the cipher state
            hitag2_cipher_reset(&tag, rx);
            // Check if the authentication was correct
            if (!hitag2_cipher_authenticate(&(tag.cs), rx + 4)) {
                // The reader failed to authenticate, do nothing
                Dbprintf("auth: %02x%02x%02x%02x%02x%02x%02x%02x Failed!", rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
                return;
            }
            // Succesful, but commented out reporting back to the Host, this may delay to much.
            // Dbprintf("auth: %02x%02x%02x%02x%02x%02x%02x%02x OK!",rx[0],rx[1],rx[2],rx[3],rx[4],rx[5],rx[6],rx[7]);

            // Activate encryption algorithm for all further communication
            tag.crypto_active = 1;

            // Use the tag password as response
            memcpy(tx, tag.sectors[3], 4);
            *txlen = 32;
        }
        break;
    }

    // LogTrace(rx, nbytes(rxlen), 0, 0, NULL, false);
    // LogTrace(tx, nbytes(txlen), 0, 0, NULL, true);

    if (tag.crypto_active) {
        hitag2_cipher_transcrypt(&(tag.cs), tx, *txlen / 8, *txlen % 8);
    }
}

// sim
static void hitag_reader_send_bit(int bit) {
    LED_A_ON();
    // Binary pulse length modulation (BPLM) is used to encode the data stream
    // This means that a transmission of a one takes longer than that of a zero

    // Enable modulation, which means, drop the field
    lf_modulation(true);

    // Wait for 4-10 times the carrier period
    lf_wait_periods(8); // wait for 4-10 times the carrier period

    // Disable modulation, just activates the field again
    lf_modulation(false);

    if (bit == 0) {
        // Zero bit: |_-|
        lf_wait_periods(12); // wait for 18-22 times the carrier period
    } else {
        // One bit: |_--|
        lf_wait_periods(22); // wait for 26-32 times the carrier period
    }
    /*lf_wait_periods(10);*/
    LED_A_OFF();
}

// sim
static void hitag_reader_send_frame(const uint8_t *frame, size_t frame_len) {
    // Send the content of the frame
    for (size_t i = 0; i < frame_len; i++) {
        hitag_reader_send_bit((frame[i / 8] >> (7 - (i % 8))) & 1);
    }
    // Enable modulation, which means, drop the field
    lf_modulation(true);
    // Wait for 4-10 times the carrier period
    lf_wait_periods(8);
    // Disable modulation, just activates the field again
    lf_modulation(false);

    // t_stop, high field for stop condition (> 36)
    lf_wait_periods(28);
}

size_t blocknr;

uint8_t hitag_crc(uint8_t *data, size_t length) {
    uint8_t crc = 0xff;
    unsigned int byte, bit;
    for (byte = 0; byte < ((length + 7) / 8); byte++) {
        crc ^= *(data + byte);
        bit = length < (8 * (byte + 1)) ? (length % 8) : 8;
        while (bit--) {
            if (crc & 0x80) {
                crc <<= 1;
                crc ^= 0x1d;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

#define test_bit(data, i) (*(data+(i/8)) >> (7-(i%8))) & 1
#define set_bit(data, i)   *(data+(i/8)) |= (1 << (7-(i%8)))
#define clear_bit(data, i) *(data+(i/8)) &= ~(1 << (7-(i%8)))
#define flip_bit(data, i)  *(data+(i/8)) ^= (1 << (7-(i%8)))
void fix_ac_decoding(uint8_t *input, size_t len) {
    // Reader routine tries to decode AC data after Manchester decoding
    // AC has double the bitrate, extract data from bit-pairs
    uint8_t temp[len / 16];
    memset(temp, 0, sizeof(temp));

    for (size_t i = 1; i < len; i += 2) {
        if (test_bit(input, i) && test_bit(input, (i + 1))) {
            set_bit(temp, (i / 2));
        }
    }
    memcpy(input, temp, sizeof(temp));
}

bool hitag_plain(uint8_t *rx, const size_t rxlen, uint8_t *tx, size_t *txlen, bool hitag_s) {
    uint8_t crc;
    *txlen = 0;
    switch (rxlen) {
        case 0: {
            // retry waking up card
            /*tx[0] = 0xb0; // Rev 3.0*/
            tx[0] = 0x30; // Rev 2.0
            *txlen = 5;
            if (!bCollision) blocknr--;
            if (blocknr < 0) {
                blocknr = 0;
            }
            if (!hitag_s) {
                if (blocknr > 1 && blocknr < 31) {
                    blocknr = 31;
                }
            }
            bCollision = true;
            return true;
        }
        case 32: {
            if (bCollision) {
                // Select card by serial from response
                tx[0] = 0x00 | rx[0] >> 5;
                tx[1] = rx[0] << 3 | rx[1] >> 5;
                tx[2] = rx[1] << 3 | rx[2] >> 5;
                tx[3] = rx[2] << 3 | rx[3] >> 5;
                tx[4] = rx[3] << 3;
                crc = hitag_crc(tx, 37);
                tx[4] |= crc >> 5;
                tx[5] = crc << 3;
                *txlen = 45;
                bCollision = false;
            } else {
                memcpy(tag.sectors[blocknr], rx, 4);
                blocknr++;
                if (!hitag_s) {
                    if (blocknr > 1 && blocknr < 31) {
                        blocknr = 31;
                    }
                }
                if (blocknr > 63) {
                    DbpString("Read succesful!");
                    *txlen = 0;
                    bSuccessful = true;
                    return false;
                }
                // read next page of card until done
                Dbprintf("Reading page %02u", blocknr);
                tx[0] = 0xc0 | blocknr >> 4; // RDPPAGE
                tx[1] = blocknr << 4;
                crc = hitag_crc(tx, 12);
                tx[1] |= crc >> 4;
                tx[2] = crc << 4;
                *txlen = 20;
            }
        }
        break;
        default: {
            Dbprintf("Uknown frame length: %d", rxlen);
            return false;
        }
        break;
    }
    return true;
}

size_t flipped_bit = 0;

uint32_t byte_value = 0;
bool hitag1_authenticate(uint8_t *rx, const size_t rxlen, uint8_t *tx, size_t *txlen) {
    uint8_t crc;
    *txlen = 0;
    switch (rxlen) {
        case 0: {
            // retry waking up card
            /*tx[0] = 0xb0; // Rev 3.0*/
            tx[0] = 0x30; // Rev 2.0
            *txlen = 5;
            if (bCrypto && byte_value <= 0xff) {
                // to retry
                bCrypto = false;
            }
            if (!bCollision) blocknr--;
            if (blocknr < 0) {
                blocknr = 0;
            }
            bCollision = true;
            // will receive 32-bit UID
        }
        break;
        case 2: {
            if (bAuthenticating) {
                // received Auth init ACK, send nonce
                // TODO Roel, bit-manipulation goes here
                /*nonce[0] = 0x2d;*/
                /*nonce[1] = 0x74;*/
                /*nonce[2] = 0x80;*/
                /*nonce[3] = 0xa5;*/
                nonce[0] = byte_value;
                byte_value++;
                /*set_bit(nonce,flipped_bit);*/
                memcpy(tx, nonce, 4);
                *txlen = 32;
                // will receive 32 bit encrypted Logdata
            } else if (bCrypto) {
                // authed, start reading
                tx[0] = 0xe0 | blocknr >> 4; // RDCPAGE
                tx[1] = blocknr << 4;
                crc = hitag_crc(tx, 12);
                tx[1] |= crc >> 4;
                tx[2] = crc << 4;
                *txlen = 20;
                // will receive 32-bit encrypted page
            }
        }
        break;
        case 32: {
            if (bCollision) {
                // Select card by serial from response
                tx[0] = 0x00 | rx[0] >> 5;
                tx[1] = rx[0] << 3 | rx[1] >> 5;
                tx[2] = rx[1] << 3 | rx[2] >> 5;
                tx[3] = rx[2] << 3 | rx[3] >> 5;
                tx[4] = rx[3] << 3;
                crc = hitag_crc(tx, 37);
                tx[4] |= crc >> 5;
                tx[5] = crc << 3;
                *txlen = 45;
                bCollision = false;
                bSelecting = true;
                // will receive 32-bit configuration page
            } else if (bSelecting) {
                // Initiate auth
                tx[0] = 0xa0 | key_no >> 4; // WRCPAGE
                tx[1] = blocknr << 4;
                crc = hitag_crc(tx, 12);
                tx[1] |= crc >> 4;
                tx[2] = crc << 4;
                *txlen = 20;
                bSelecting = false;
                bAuthenticating = true;
                // will receive 2-bit ACK
            } else if (bAuthenticating) {
                // received 32-bit logdata 0
                // TODO decrypt logdata 0, verify against logdata_0
                memcpy(tag.sectors[0], rx, 4);
                memcpy(tag.sectors[1], tx, 4);
                Dbprintf("%02x%02x%02x%02x %02x%02x%02x%02x", rx[0], rx[1], rx[2], rx[3], tx[0], tx[1], tx[2], tx[3]);
                // TODO replace with secret data stream
                // TODO encrypt logdata_1
                memcpy(tx, logdata_1, 4);
                *txlen = 32;
                bAuthenticating = false;
                bCrypto = true;
                // will receive 2-bit ACK
            } else if (bCrypto) {
                // received 32-bit encrypted page
                // TODO decrypt rx
                memcpy(tag.sectors[blocknr], rx, 4);
                blocknr++;
                if (blocknr > 63) {
                    DbpString("Read succesful!");
                    bSuccessful = true;
                    return false;
                }

                // TEST
                Dbprintf("Succesfully authenticated with logdata:");
                Dbhexdump(4, logdata_1, false);
                bSuccessful = true;
                return false;

                // read next page of card until done
                tx[0] = 0xe0 | blocknr >> 4; // RDCPAGE
                tx[1] = blocknr << 4;
                crc = hitag_crc(tx, 12);
                tx[1] |= crc >> 4;
                tx[2] = crc << 4;
                *txlen = 20;
            }
        }
        break;
        default: {
            Dbprintf("Uknown frame length: %d", rxlen);
            return false;
        }
        break;
    }

    return true;
}

//-----------------------------------------------------------------------------
// Hitag2 operations
//-----------------------------------------------------------------------------

static bool hitag2_write_page(uint8_t *rx, const size_t rxlen, uint8_t *tx, size_t *txlen) {
    switch (writestate) {
        case WRITE_STATE_START:
            *txlen = 10;
            tx[0] = 0x82 | (blocknr << 3) | ((blocknr ^ 7) >> 2);
            tx[1] = ((blocknr ^ 7) << 6);
            writestate = WRITE_STATE_PAGENUM_WRITTEN;
            break;
        case WRITE_STATE_PAGENUM_WRITTEN:
            // Check if page number was received correctly
            if ((rxlen == 10)
                    && (rx[0] == (0x82 | (blocknr << 3) | ((blocknr ^ 7) >> 2)))
                    && (rx[1] == (((blocknr & 0x3) ^ 0x3) << 6))) {

                *txlen = 32;
                memset(tx, 0, HITAG_FRAME_LEN);
                memcpy(tx, writedata, 4);
                writestate = WRITE_STATE_PROG;
            } else {
                Dbprintf("hitag2_write_page: Page number was not received correctly: rxlen=%d rx=%02x%02x%02x%02x",
                         rxlen, rx[0], rx[1], rx[2], rx[3]);
                bSuccessful = false;
                return false;
            }
            break;
        case WRITE_STATE_PROG:
            if (rxlen == 0) {
                bSuccessful = true;
            } else {
                bSuccessful = false;
                Dbprintf("hitag2_write_page: unexpected rx data (%d) after page write", rxlen);
            }
            return false;
        default:
            DbpString("hitag2_write_page: Unknown state %d");
            bSuccessful = false;
            return false;
    }

    return true;
}

static bool hitag2_password(uint8_t *rx, const size_t rxlen, uint8_t *tx, size_t *txlen, bool write) {
    // Reset the transmission frame length
    *txlen = 0;

    if (bPwd && !bAuthenticating && write) {
        if (!hitag2_write_page(rx, rxlen, tx, txlen)) {
            return false;
        }
    } else {
        // Try to find out which command was send by selecting on length (in bits)
        switch (rxlen) {
            // No answer, try to resurrect
            case 0: {
                // Stop if there is no answer (after sending password)
                if (bPwd) {
                    DbpString("Password failed!");
                    return false;
                }
                *txlen = 5;
                memcpy(tx, "\xC0", nbytes(*txlen));
            }
            break;

            // Received UID, tag password
            case 32: {
                // stage 1, got UID
                if (!bPwd) {
                    bPwd = true;
                    bAuthenticating = true;
                    memcpy(tx, password, 4);
                    *txlen = 32;
                } else {
                    // stage 2, got config byte+password TAG, discard as will read later
                    if (bAuthenticating) {
                        bAuthenticating = false;
                        if (write) {
                            if (!hitag2_write_page(rx, rxlen, tx, txlen)) {
                                return false;
                            }
                            break;
                        }
                    }
                    // stage 2+, got data block
                    else {
                        memcpy(tag.sectors[blocknr], rx, 4);
                        blocknr++;
                    }

                    if (blocknr > 7) {
                        DbpString("Read successful!");
                        bSuccessful = true;
                        return false;
                    }
                    *txlen = 10;
                    tx[0] = 0xC0 | (blocknr << 3) | ((blocknr ^ 7) >> 2);
                    tx[1] = ((blocknr ^ 7) << 6);
                }
            }
            break;

            // Unexpected response
            default: {
                Dbprintf("Unknown frame length: %d", rxlen);
                return false;
            }
            break;
        }
    }

    return true;
}

static bool hitag2_crypto(uint8_t *rx, const size_t rxlen, uint8_t *tx, size_t *txlen, bool write) {
    // Reset the transmission frame length
    *txlen = 0;

    if (bCrypto) {
        hitag2_cipher_transcrypt(&cipher_state, rx, rxlen / 8, rxlen % 8);
    }

    if (bCrypto && !bAuthenticating && write) {
        if (!hitag2_write_page(rx, rxlen, tx, txlen)) {
            return false;
        }
    } else {

        // Try to find out which command was send by selecting on length (in bits)
        switch (rxlen) {
            // No answer, try to resurrect
            case 0: {
                // Stop if there is no answer while we are in crypto mode (after sending NrAr)
                if (bCrypto) {
                    // Failed during authentication
                    if (bAuthenticating) {
                        DbpString("Authentication failed!");
                        return false;
                    } else {
                        // Failed reading a block, could be (read/write) locked, skip block and re-authenticate
                        if (blocknr == 1) {
                            // Write the low part of the key in memory
                            memcpy(tag.sectors[1], key + 2, 4);
                        } else if (blocknr == 2) {
                            // Write the high part of the key in memory
                            tag.sectors[2][0] = 0x00;
                            tag.sectors[2][1] = 0x00;
                            tag.sectors[2][2] = key[0];
                            tag.sectors[2][3] = key[1];
                        } else {
                            // Just put zero's in the memory (of the unreadable block)
                            memset(tag.sectors[blocknr], 0x00, 4);
                        }
                        blocknr++;
                        bCrypto = false;
                    }
                } else {
                    *txlen = 5;
                    memcpy(tx, "\xc0", nbytes(*txlen));
                }
                break;
            }
            // Received UID, crypto tag answer
            case 32: {
                // stage 1, got UID
                if (!bCrypto) {
                    uint64_t ui64key = key[0] | ((uint64_t)key[1]) << 8 | ((uint64_t)key[2]) << 16 | ((uint64_t)key[3]) << 24 | ((uint64_t)key[4]) << 32 | ((uint64_t)key[5]) << 40;
                    uint32_t ui32uid = rx[0] | ((uint32_t)rx[1]) << 8 | ((uint32_t)rx[2]) << 16 | ((uint32_t)rx[3]) << 24;
                    Dbprintf("hitag2_crypto: key=0x%x%x uid=0x%x", (uint32_t)((REV64(ui64key)) >> 32), (uint32_t)((REV64(ui64key)) & 0xffffffff), REV32(ui32uid));
                    cipher_state = _hitag2_init(REV64(ui64key), REV32(ui32uid), 0);
                    // PRN
                    memset(tx, 0x00, 4);
                    // Secret data
                    memset(tx + 4, 0xff, 4);
                    hitag2_cipher_transcrypt(&cipher_state, tx + 4, 4, 0);
                    *txlen = 64;
                    bCrypto = true;
                    bAuthenticating = true;
                } else {
                    // stage 2, got config byte+password TAG, discard as will read later
                    if (bAuthenticating) {
                        bAuthenticating = false;
                        if (write) {
                            if (!hitag2_write_page(rx, rxlen, tx, txlen)) {
                                return false;
                            }
                            break;
                        }
                    }
                    // stage 2+, got data block
                    else {
                        // Store the received block
                        memcpy(tag.sectors[blocknr], rx, 4);
                        blocknr++;
                    }
                    if (blocknr > 7) {
                        DbpString("Read successful!");
                        bSuccessful = true;
                        return false;
                    } else {
                        *txlen = 10;
                        tx[0] = 0xc0 | (blocknr << 3) | ((blocknr ^ 7) >> 2);
                        tx[1] = ((blocknr ^ 7) << 6);
                    }
                }
            }
            break;

            // Unexpected response
            default: {
                Dbprintf("Unknown frame length: %d", rxlen);
                return false;
            }
            break;
        }
    }

    if (bCrypto) {
        // We have to return now to avoid double encryption
        if (!bAuthenticating) {
            hitag2_cipher_transcrypt(&cipher_state, tx, *txlen / 8, *txlen % 8);
        }
    }

    return true;
}

static bool hitag2_authenticate(uint8_t *rx, const size_t rxlen, uint8_t *tx, size_t *txlen) {
    // Reset the transmission frame length
    *txlen = 0;

    // Try to find out which command was send by selecting on length (in bits)
    switch (rxlen) {
        // No answer, try to resurrect
        case 0: {
            // Stop if there is no answer while we are in crypto mode (after sending NrAr)
            if (bCrypto) {
                DbpString("Authentication failed!");
                return false;
            }
            *txlen = 5;
            memcpy(tx, "\xC0", nbytes(*txlen));
        }
        break;

        // Received UID, crypto tag answer
        case 32: {
            if (!bCrypto) {
                *txlen = 64;
                memcpy(tx, NrAr, 8);
                bCrypto = true;
            } else {
                DbpString("Authentication successful!");
                return true;
            }
        }
        break;

        // Unexpected response
        default: {
            Dbprintf("Unknown frame length: %d", rxlen);
            return false;
        }
        break;
    }

    return true;
}

static bool hitag2_test_auth_attempts(uint8_t *rx, const size_t rxlen, uint8_t *tx, size_t *txlen) {

    // Reset the transmission frame length
    *txlen = 0;

    // Try to find out which command was send by selecting on length (in bits)
    switch (rxlen) {
        // No answer, try to resurrect
        case 0: {
            // Stop if there is no answer while we are in crypto mode (after sending NrAr)
            if (bCrypto) {
                Dbprintf("auth: %02x%02x%02x%02x%02x%02x%02x%02x Failed, removed entry!", NrAr[0], NrAr[1], NrAr[2], NrAr[3], NrAr[4], NrAr[5], NrAr[6], NrAr[7]);

                // Removing failed entry from authentiations table
                memcpy(auth_table + auth_table_pos, auth_table + auth_table_pos + 8, 8);
                auth_table_len -= 8;

                // Return if we reached the end of the authentications table
                bCrypto = false;
                if (auth_table_pos == auth_table_len) {
                    return false;
                }

                // Copy the next authentication attempt in row (at the same position, b/c we removed last failed entry)
                memcpy(NrAr, auth_table + auth_table_pos, 8);
            }
            *txlen = 5;
            memcpy(tx, "\xc0", nbytes(*txlen));
        }
        break;

        // Received UID, crypto tag answer, or read block response
        case 32: {
            if (!bCrypto) {
                *txlen = 64;
                memcpy(tx, NrAr, 8);
                bCrypto = true;
            } else {
                Dbprintf("auth: %02x%02x%02x%02x%02x%02x%02x%02x OK", NrAr[0], NrAr[1], NrAr[2], NrAr[3], NrAr[4], NrAr[5], NrAr[6], NrAr[7]);
                bCrypto = false;
                if ((auth_table_pos + 8) == auth_table_len) {
                    return false;
                }
                auth_table_pos += 8;
                memcpy(NrAr, auth_table + auth_table_pos, 8);
            }
        }
        break;

        default: {
            Dbprintf("Unknown frame length: %d", rxlen);
            return false;
        }
        break;
    }

    return true;
}

static bool hitag2_read_uid(uint8_t *rx, const size_t rxlen, uint8_t *tx, size_t *txlen) {
    // Reset the transmission frame length
    *txlen = 0;

    // Try to find out which command was send by selecting on length (in bits)
    switch (rxlen) {
        // No answer, try to resurrect
        case 0: {
            // Just starting or if there is no answer
            *txlen = 5;
            memcpy(tx, "\xC0", nbytes(*txlen));
        }
        break;
        // Received UID
        case 32: {
            // Check if we received answer tag (at)
            if (bAuthenticating) {
                bAuthenticating = false;
            } else {
                // Store the received block
                memcpy(tag.sectors[blocknr], rx, 4);
                blocknr++;

                Dbhexdump(4, rx, false);
            }
            if (blocknr > 0) {
                DbpString("Read successful!");
                bSuccessful = true;
                return false;
            }
        }
        break;
        // Unexpected response
        default: {
            Dbprintf("Unknown frame length: %d", rxlen);
            return false;
        }
        break;
    }
    return true;
}

// Hitag2 Sniffing
void SniffHitag(void) {

    LEDsoff();
    StopTicks();

    FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
    BigBuf_free();
    BigBuf_Clear_ext(false);
    clear_trace();
    set_tracing(true);

    auth_table_len = 0;
    auth_table_pos = 0;

    auth_table = (uint8_t *)BigBuf_malloc(AUTH_TABLE_LENGTH);
    memset(auth_table, 0x00, AUTH_TABLE_LENGTH);

    DbpString("Starting Hitag2 sniffing");
    LED_D_ON();

    lf_init(false);
    logging = false;

    size_t periods = 0;
    uint8_t periods_bytes[4];

    /*bool waiting_for_first_edge = true;*/
    LED_C_ON();

    while (!BUTTON_PRESS() && !data_available()) {

        WDT_HIT();

        // Receive frame, watch for at most T0*EOF periods
        lf_reset_counter();

        // Wait "infinite" for reader modulation
        periods = lf_detect_gap(20000);

        // Test if we detected the first reader modulation edge
        if (periods != 0) {
            if (logging == false) {
                logging = true;
                LED_D_ON();
            }
        }

        /*lf_count_edge_periods(10000);*/
        while ((periods = lf_detect_gap(64)) != 0) {
            num_to_bytes(periods, 4, periods_bytes);
            LogTrace(periods_bytes, 4, 0, 0, NULL, true);
        }


        /*
                // Check if frame was captured
                if (rxlen > 0) {
                    // frame_count++;
                    LogTrace(rx, nbytes(rxlen), response, 0, NULL, reader_frame);

                    // Check if we recognize a valid authentication attempt
                    if (nbytes(rxlen) == 8) {
                        // Store the authentication attempt
                        if (auth_table_len < (AUTH_TABLE_LENGTH - 8)) {
                            memcpy(auth_table + auth_table_len, rx, 8);
                            auth_table_len += 8;
                        }
                    }
        */
    }

    lf_finalize();

    StartTicks();

    DbpString("Hitag2 sniffing finish. Use `lf hitag list` for annotations");
}

// Hitag2 simulation
void SimulateHitagTag(bool tag_mem_supplied, uint8_t *data) {

    StopTicks();

    // int frame_count = 0;
    int response = 0, overflow = 0;
    uint8_t rx[HITAG_FRAME_LEN];
    size_t rxlen = 0;
    uint8_t tx[HITAG_FRAME_LEN];
    size_t txlen = 0;

    FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
    BigBuf_free();
    BigBuf_Clear_ext(false);
    clear_trace();
    set_tracing(true);

    auth_table_len = 0;
    auth_table_pos = 0;
    auth_table = BigBuf_malloc(AUTH_TABLE_LENGTH);
    memset(auth_table, 0x00, AUTH_TABLE_LENGTH);

    // Reset the received frame, frame count and timing info
    memset(rx, 0x00, sizeof(rx));

    DbpString("Starting Hitag2 simulation");

    LED_D_ON();
    hitag2_init();

    if (tag_mem_supplied) {
        DbpString("Loading hitag2 memory...");
        memcpy((uint8_t *)tag.sectors, data, 48);
    }

    uint32_t block = 0;
    for (size_t i = 0; i < 12; i++) {
        for (size_t j = 0; j < 4; j++) {
            block <<= 8;
            block |= tag.sectors[i][j];
        }
        Dbprintf("| %d | %08x |", i, block);
    }

    // Set up simulator mode, frequency divisor which will drive the FPGA
    // and analog mux selection.
    FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_EDGE_DETECT);
    FpgaSendCommand(FPGA_CMD_SET_DIVISOR, LF_DIVISOR_125); //125kHz
    SetAdcMuxFor(GPIO_MUXSEL_LOPKD);

    // Configure output pin that is connected to the FPGA (for modulating)
    AT91C_BASE_PIOA->PIO_OER |= GPIO_SSC_DOUT;
    AT91C_BASE_PIOA->PIO_PER |= GPIO_SSC_DOUT;

    // Disable modulation at default, which means release resistance
    LOW(GPIO_SSC_DOUT);

    // Enable Peripheral Clock for
    //   TIMER_CLOCK0, used to measure exact timing before answering
    //   TIMER_CLOCK1, used to capture edges of the tag frames
    AT91C_BASE_PMC->PMC_PCER |= (1 << AT91C_ID_TC0) | (1 << AT91C_ID_TC1);

    AT91C_BASE_PIOA->PIO_BSR = GPIO_SSC_FRAME;

    // Disable timer during configuration
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKDIS;
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

    // TC0: Capture mode, default timer source = MCK/2 (TIMER_CLOCK1), no triggers
    AT91C_BASE_TC0->TC_CMR = AT91C_TC_CLKS_TIMER_DIV1_CLOCK;

    // TC1: Capture mode, default timer source = MCK/2 (TIMER_CLOCK1), TIOA is external trigger,
    // external trigger rising edge, load RA on rising edge of TIOA.
    AT91C_BASE_TC1->TC_CMR = AT91C_TC_CLKS_TIMER_DIV1_CLOCK | AT91C_TC_ETRGEDG_RISING | AT91C_TC_ABETRG | AT91C_TC_LDRA_RISING;

    // Enable and reset counter
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

    // synchronized startup procedure
    while (AT91C_BASE_TC0->TC_CV > 0) {}; // wait until TC0 returned to zero

    while (!BUTTON_PRESS() && !data_available()) {
        // Watchdog hit
        WDT_HIT();

        // Receive frame, watch for at most T0*EOF periods
        while (AT91C_BASE_TC1->TC_CV < T0 * HITAG_T_EOF) {
            // Check if rising edge in modulation is detected
            if (AT91C_BASE_TC1->TC_SR & AT91C_TC_LDRAS) {
                // Retrieve the new timing values
                int ra = (AT91C_BASE_TC1->TC_RA / T0) + overflow;
                overflow = 0;

                // Reset timer every frame, we have to capture the last edge for timing
                AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

                LED_B_ON();

                // Capture reader frame
                if (ra >= HITAG_T_STOP) {
                    if (rxlen != 0) {
                        //DbpString("wierd0?");
                    }
                    // Capture the T0 periods that have passed since last communication or field drop (reset)
                    response = (ra - HITAG_T_LOW);
                } else if (ra >= HITAG_T_1_MIN) {
                    // '1' bit
                    rx[rxlen / 8] |= 1 << (7 - (rxlen % 8));
                    rxlen++;
                } else if (ra >= HITAG_T_0_MIN) {
                    // '0' bit
                    rx[rxlen / 8] |= 0 << (7 - (rxlen % 8));
                    rxlen++;
                } else {
                    // Ignore wierd value, is to small to mean anything
                }
            }
        }

        // Check if frame was captured
        if (rxlen > 4) {
            // frame_count++;
            LogTrace(rx, nbytes(rxlen), response, response, NULL, true);

            // Disable timer 1 with external trigger to avoid triggers during our own modulation
            AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

            // Process the incoming frame (rx) and prepare the outgoing frame (tx)
            hitag2_handle_reader_command(rx, rxlen, tx, &txlen);

            // Wait for HITAG_T_WAIT_1 carrier periods after the last reader bit,
            // not that since the clock counts since the rising edge, but T_Wait1 is
            // with respect to the falling edge, we need to wait actually (T_Wait1 - T_Low)
            // periods. The gap time T_Low varies (4..10). All timer values are in
            // terms of T0 units
            while (AT91C_BASE_TC0->TC_CV < T0 * (HITAG_T_WAIT_1 - HITAG_T_LOW));

            // Send and store the tag answer (if there is any)
            if (txlen) {
                hitag_send_frame(tx, txlen);
                LogTrace(tx, nbytes(txlen), 0, 0, NULL, false);
            }

            // Reset the received frame and response timing info
            memset(rx, 0x00, sizeof(rx));
            response = 0;

            // Enable and reset external trigger in timer for capturing future frames
            AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;
            LED_B_OFF();
        }
        // Reset the frame length
        rxlen = 0;
        // Save the timer overflow, will be 0 when frame was received
        overflow += (AT91C_BASE_TC1->TC_CV / T0);
        // Reset the timer to restart while-loop that receives frames
        AT91C_BASE_TC1->TC_CCR = AT91C_TC_SWTRG;
    }

    LEDsoff();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    set_tracing(false);
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKDIS;

    // release allocated memory from BigBuff.
    BigBuf_free();

    StartTicks();

    DbpString("Sim Stopped");
}

void ReaderHitag(hitag_function htf, hitag_data *htd) {

    StopTicks();

    int frame_count = 0;
    int response;
    uint8_t rx[HITAG_FRAME_LEN];
    size_t rxlen = 0;
    uint8_t txbuf[HITAG_FRAME_LEN];
    uint8_t *tx = txbuf;
    size_t txlen = 0;
    int t_wait_1;
    int t_wait_2;
    size_t tag_size;
    bool bStop = false;

    // Raw demodulation/decoding by sampling edge periods
    size_t periods = 0;

    // Reset the return status
    bSuccessful = false;
    bCrypto = false;

    // Clean up trace and prepare it for storing frames
    set_tracing(true);
    clear_trace();

    DbpString("Starting Hitag reader family");

    // Check configuration
    switch (htf) {
        case RHT1F_PLAIN: {
            Dbprintf("Read public blocks in plain mode");
            // this part will be unreadable
            memset(tag.sectors + 2, 0x0, 30);
            blocknr = 0;
        }
        break;

        case RHT1F_AUTHENTICATE: {
            Dbprintf("Read all blocks in authed mode");
            memcpy(nonce, htd->ht1auth.nonce, 4);
            memcpy(key, htd->ht1auth.key, 4);
            memcpy(logdata_0, htd->ht1auth.logdata_0, 4);
            memcpy(logdata_1, htd->ht1auth.logdata_1, 4);
            // TEST
            memset(nonce, 0x0, 4);
            memset(logdata_1, 0x00, 4);
            byte_value = 0;
            key_no = htd->ht1auth.key_no;
            Dbprintf("Authenticating using key #%d:", key_no);
            Dbhexdump(4, key, false);
            DbpString("Nonce:");
            Dbhexdump(4, nonce, false);
            DbpString("Logdata_0:");
            Dbhexdump(4, logdata_0, false);
            DbpString("Logdata_1:");
            Dbhexdump(4, logdata_1, false);
            blocknr = 0;
        }
        break;
        case RHT2F_PASSWORD: {
            Dbprintf("List identifier in password mode");
            memcpy(password, htd->pwd.password, 4);
            blocknr = 0;
            bPwd = false;
            bAuthenticating = false;
            break;
        }
        case RHT2F_AUTHENTICATE: {
            DbpString("Authenticating using nr,ar pair:");
            memcpy(NrAr, htd->auth.NrAr, 8);
            Dbhexdump(8, NrAr, false);
            bCrypto = false;
            bAuthenticating = false;
            break;
        }
        case RHT2F_CRYPTO: {
            DbpString("Authenticating using key:");
            memcpy(key, htd->crypto.key, 6);  //HACK; 4 or 6??  I read both in the code.
            Dbhexdump(6, key, false);
            DbpString("Nonce:");
            Dbhexdump(4, nonce, false);
            memcpy(nonce, htd->crypto.data, 4);
            blocknr = 0;
            bCrypto = false;
            bAuthenticating = false;
            break;
        }
        case RHT2F_TEST_AUTH_ATTEMPTS: {
            Dbprintf("Testing %d authentication attempts", (auth_table_len / 8));
            auth_table_pos = 0;
            memcpy(NrAr, auth_table, 8);
            bCrypto = false;
            break;
        }
        case RHT2F_UID_ONLY: {
            blocknr = 0;
            bCrypto = false;
            bAuthenticating = false;
            break;
        }
        default: {
            Dbprintf("Error, unknown function: %d", htf);
            set_tracing(false);
            StartTicks();
            return;
        }
    }

    LED_D_ON();
    hitag2_init();

    // init as reader
    lf_init(true);

    uint8_t attempt_count = 0;

    // Tag specific configuration settings (sof, timings, etc.)
    if (htf < 10) {
        // hitagS settings
        t_wait_1 = 204;
        t_wait_2 = 128;
        /*tag_size = 256;*/
        flipped_bit = 0;
        tag_size = 8;
        DbpString("Configured for hitagS reader");
    } else if (htf < 20) {
        // hitag1 settings
        t_wait_1 = 204;
        t_wait_2 = 128;
        tag_size = 256;
        flipped_bit = 0;
        DbpString("Configured for hitag1 reader");
    } else if (htf < 30) {
        // hitag2 settings
        t_wait_1 = 206;
        t_wait_2 = 90;
        tag_size = 48;
        DbpString("Configured for hitag2 reader");
    } else {
        Dbprintf("Error, unknown hitag reader type: %d", htf);
        return;
    }

    uint8_t tag_modulation;
    size_t max_nrzs = (8 * HITAG_FRAME_LEN + 5) * 2; // up to 2 nrzs per bit
    uint8_t nrz_samples[max_nrzs];
    size_t nrzs = 0;

    while (!bStop && !BUTTON_PRESS() && !data_available()) {

        WDT_HIT();

        // Check if frame was captured and store it
        if (rxlen > 0) {
            frame_count++;
            response++;
            LogTrace(rx, nbytes(rxlen), response, response, NULL, false);
            //Dbhexdump(nbytes(rxlen), rx, false);
        }

        // By default reset the transmission buffer
        tx = txbuf;
        switch (htf) {
            case RHT1F_PLAIN: {
                bStop = !hitag_plain(rx, rxlen, tx, &txlen, false);
            }
            break;

            case RHT1F_AUTHENTICATE: {
                bStop = !hitag1_authenticate(rx, rxlen, tx, &txlen);
            }
            break;

            case RHT2F_PASSWORD: {
                bStop = !hitag2_password(rx, rxlen, tx, &txlen, false);
                break;
            }
            case RHT2F_AUTHENTICATE: {
                bStop = !hitag2_authenticate(rx, rxlen, tx, &txlen);
                break;
            }
            case RHT2F_CRYPTO: {
                bStop = !hitag2_crypto(rx, rxlen, tx, &txlen, false);
                break;
            }
            case RHT2F_TEST_AUTH_ATTEMPTS: {
                bStop = !hitag2_test_auth_attempts(rx, rxlen, tx, &txlen);
                break;
            }
            case RHT2F_UID_ONLY: {
                bStop = !hitag2_read_uid(rx, rxlen, tx, &txlen);
                attempt_count++; //attempt 3 times to get uid then quit
                if (!bStop && attempt_count == 3)
                    bStop = true;

                break;
            }
            default: {
                Dbprintf("Error, unknown function: %d", htf);
                goto out;
            }
        }

        // Wait for t_wait_2 carrier periods after the last tag bit before transmitting,
        lf_wait_periods(t_wait_2);

        // Transmit the reader frame
        hitag_reader_send_frame(tx, txlen);

        // Let the antenna and ADC values settle
        // And find the position where edge sampling should start
        lf_wait_periods(t_wait_1 - 64);

        // Reset the response time (in number of periods)
        response = 0;

        // Keep administration of the first edge detection
        bool waiting_for_first_edge = true;

        // Did we detected any modulaiton at all
        bool detected_tag_modulation = false;

        // Use the current modulation state as starting point
        tag_modulation = lf_get_tag_modulation();

        // Reset the number of NRZ samples and use edge detection to detect them
        nrzs = 0;
        while (nrzs < max_nrzs) {
            // Get the timing of the next edge in number of wave periods
            periods = lf_count_edge_periods(128);

            // Are we dealing with the first incoming edge
            if (waiting_for_first_edge) {
                // Just break out of loop after an initial time-out (tag is probably not available)
                if (periods == 0) break;
                // Register the number of periods that have passed
                response = t_wait_1 - 64 + periods;
                // Indicate that we have dealt with the first edge
                waiting_for_first_edge = false;
                // The first edge is always a single NRZ bit, force periods on 16
                periods = 16;
                // We have received more than 0 periods, so we have detected a tag response
                detected_tag_modulation = true;
            } else {
                // The function lf_count_edge_periods() returns 0 when a time-out occurs
                if (periods == 0) {
                    Dbprintf("Detected timeout after [%d] nrz samples", nrzs);
                    break;
                }
            }

            // Evaluate the number of periods before the next edge
            if (periods > 24 && periods <= 64) {
                // Detected two sequential equal bits and a modulation switch
                // NRZ modulation: (11 => --|) or (11 __|)
                nrz_samples[nrzs++] = tag_modulation;
                nrz_samples[nrzs++] = tag_modulation;
                // Invert tag modulation state
                tag_modulation ^= 1;
            } else if (periods > 0 && periods <= 24) {
                // Detected one bit and a modulation switch
                // NRZ modulation: (1 => -|) or (0 _|)
                nrz_samples[nrzs++] = tag_modulation;
                tag_modulation ^= 1;
            } else {
                // The function lf_count_edge_periods() returns > 64 periods, this is not a valid number periods
                Dbprintf("Detected unexpected period count: %d", periods);
                break;
            }
        }

        // Store the TX frame, we do this now at this point, to avoid delay in processing
        // and to be able to overwrite the first samples with the trace (since they currently
        // still use the same memory space)
        if (txlen > 0) {
            frame_count++;
            LogTrace(tx, nbytes(txlen), HITAG_T_WAIT_2, HITAG_T_WAIT_2, NULL, true);
        }

        // Reset values for receiving frames
        memset(rx, 0x00, sizeof(rx));
        rxlen = 0;

        // If there is no response, just repeat the loop
        if (!detected_tag_modulation) continue;

        // Make sure we always have an even number of samples. This fixes the problem
        // of ending the manchester decoding with a zero. See the example below where
        // the '|' character is end of modulation
        //  One at the end: ..._-|_____...
        // Zero at the end: ...-_|_____...
        // The last modulation change of a zero is not detected, but we should take
        // the half period in account, otherwise the demodulator will fail.
        if ((nrzs % 2) != 0) {
            nrz_samples[nrzs++] = tag_modulation;
        }

        LED_B_ON();

        // decode bitstream
        manrawdecode((uint8_t *)nrz_samples, &nrzs, true, 0);

        // decode frame

        // Verify if the header consists of five consecutive ones
        if (nrzs < 5) {
            Dbprintf("Detected unexpected number of manchester decoded samples [%d]", nrzs);
            break;
        } else {
            for (size_t i = 0; i < 5; i++) {
                if (nrz_samples[i] != 1) {
                    Dbprintf("Detected incorrect header, the bit [%d] is zero instead of one", i);
                }
            }
        }

        // Pack the response into a byte array
        for (size_t i = 5; i < nrzs; i++) {
            uint8_t bit = nrz_samples[i];
            rx[rxlen / 8] |= bit << (7 - (rxlen % 8));
            rxlen++;
        }
        if (rxlen % 8 == 1) // skip spurious bit
            rxlen--;

        // Check if frame was captured and store it
        if (rxlen > 0) {
            frame_count++;
//            if (bCollision){
//                // AC decoding hack
//                fix_ac_decoding(rx, 64);
//                rxlen = 32;
//            }

            LogTrace(rx, nbytes(rxlen), response, 0, NULL, false);
            Dbhexdump(nbytes(rxlen), rx, false);
        }
    }

out:
    lf_finalize();
    Dbprintf("frame received: %u", frame_count);

    // release allocated memory from BigBuff.
    BigBuf_free();
    StartTicks();

    if (bSuccessful)
        reply_old(CMD_ACK, bSuccessful, 0, 0, (uint8_t *)tag.sectors, tag_size);
    else
        reply_mix(CMD_ACK, bSuccessful, 0, 0, 0, 0);
}

void WriterHitag(hitag_function htf, hitag_data *htd, int page) {

    StopTicks();

    // int frame_count = 0;
    int response = 0;
    uint8_t rx[HITAG_FRAME_LEN];
    size_t rxlen = 0;
    uint8_t txbuf[HITAG_FRAME_LEN];
    uint8_t *tx = txbuf;
    size_t txlen = 0;
    int lastbit;
    int reset_sof;
    int t_wait = HITAG_T_WAIT_MAX;
    bool bStop;

    FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
    set_tracing(true);
    clear_trace();

    // Reset the return status
    bSuccessful = false;

    // Check configuration
    switch (htf) {
        case WHT2F_CRYPTO: {
            DbpString("Authenticating using key:");
            memcpy(key, htd->crypto.key, 6); //HACK; 4 or 6??  I read both in the code.
            memcpy(writedata, htd->crypto.data, 4);
            Dbhexdump(6, key, false);
            blocknr = page;
            bCrypto = false;
            bAuthenticating = false;
            writestate = WRITE_STATE_START;
        }
        break;
        case WHT2F_PASSWORD: {
            DbpString("Authenticating using password:");
            memcpy(password, htd->pwd.password, 4);
            memcpy(writedata, htd->crypto.data, 4);
            Dbhexdump(4, password, false);
            blocknr = page;
            bPwd = false;
            bAuthenticating = false;
            writestate = WRITE_STATE_START;
        }
        break;
        default: {
            Dbprintf("Error, unknown function: %d", htf);
            StartTicks();
            return;
        }
        break;
    }

    LED_D_ON();
    hitag2_init();

    // Configure output and enable pin that is connected to the FPGA (for modulating)
    AT91C_BASE_PIOA->PIO_OER |= GPIO_SSC_DOUT;
    AT91C_BASE_PIOA->PIO_PER |= GPIO_SSC_DOUT;

    // Set fpga in edge detect with reader field, we can modulate as reader now
    FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_EDGE_DETECT | FPGA_LF_EDGE_DETECT_READER_FIELD);
    FpgaSendCommand(FPGA_CMD_SET_DIVISOR, LF_DIVISOR_125); //125kHz
    SetAdcMuxFor(GPIO_MUXSEL_LOPKD);

    // Disable modulation at default, which means enable the field
    LOW(GPIO_SSC_DOUT);

    // Enable Peripheral Clock for
    //   TIMER_CLOCK0, used to measure exact timing before answering
    //   TIMER_CLOCK1, used to capture edges of the tag frames
    AT91C_BASE_PMC->PMC_PCER |= (1 << AT91C_ID_TC0) | (1 << AT91C_ID_TC1);

    AT91C_BASE_PIOA->PIO_BSR = GPIO_SSC_FRAME;

    // Disable timer during configuration
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKDIS;
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

    // TC0: Capture mode, default timer source = MCK/2 (TIMER_CLOCK1), no triggers
    AT91C_BASE_TC0->TC_CMR = AT91C_TC_CLKS_TIMER_DIV1_CLOCK;

    // TC1: Capture mode, defaul timer source = MCK/2 (TIMER_CLOCK1), TIOA is external trigger,
    // external trigger rising edge, load RA on falling edge of TIOA.
    AT91C_BASE_TC1->TC_CMR = AT91C_TC_CLKS_TIMER_DIV1_CLOCK
                             | AT91C_TC_ETRGEDG_FALLING
                             | AT91C_TC_ABETRG
                             | AT91C_TC_LDRA_FALLING;

    // Enable and reset counters
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;
    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

    while (AT91C_BASE_TC0->TC_CV > 0) {};

    // Reset the received frame, frame count and timing info
    lastbit = 1;
    bStop = false;

    // Tag specific configuration settings (sof, timings, etc.)
    if (htf < 10) {
        // hitagS settings
        reset_sof = 1;
        t_wait = 200;
    } else if (htf < 20) {
        // hitag1 settings
        reset_sof = 1;
        t_wait = 200;
    } else if (htf < 30) {
        // hitag2 settings
        reset_sof = 4;
        t_wait = HITAG_T_WAIT_2;
    } else {
        Dbprintf("Error, unknown hitag reader type: %d", htf);
        return;
    }

    while (!bStop && !BUTTON_PRESS() && !data_available()) {

        WDT_HIT();

        // Check if frame was captured and store it
        if (rxlen > 0) {
            // frame_count++;
            LogTrace(rx, nbytes(rxlen), response, response, NULL, false);
        }

        // By default reset the transmission buffer
        tx = txbuf;
        switch (htf) {
            case WHT2F_CRYPTO: {
                bStop = !hitag2_crypto(rx, rxlen, tx, &txlen, true);
            }
            break;
            case WHT2F_PASSWORD: {
                bStop = !hitag2_password(rx, rxlen, tx, &txlen, true);
            }
            break;
            default: {
                Dbprintf("Error, unknown function: %d", htf);
                return;
            }
            break;
        }

        // Send and store the reader command
        // Disable timer 1 with external trigger to avoid triggers during our own modulation
        AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;

        // Wait for HITAG_T_WAIT_2 carrier periods after the last tag bit before transmitting,
        // Since the clock counts since the last falling edge, a 'one' means that the
        // falling edge occurred halfway the period. with respect to this falling edge,
        // we need to wait (T_Wait2 + half_tag_period) when the last was a 'one'.
        // All timer values are in terms of T0 units
        while (AT91C_BASE_TC0->TC_CV < T0 * (t_wait + (HITAG_T_TAG_HALF_PERIOD * lastbit))) {};

        // Transmit the reader frame
        hitag_reader_send_frame(tx, txlen);

        // Enable and reset external trigger in timer for capturing future frames
        AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKEN | AT91C_TC_SWTRG;

        // Add transmitted frame to total count
        if (txlen > 0) {
            // frame_count++;
            LogTrace(tx, nbytes(txlen), HITAG_T_WAIT_2, HITAG_T_WAIT_2, NULL, true);
        }

        // Reset values for receiving frames
        memset(rx, 0x00, sizeof(rx));
        rxlen = 0;
        lastbit = 1;
        bool bSkip = true;
        int tag_sof = reset_sof;
        response = 0;
        uint32_t errorCount = 0;

        // Receive frame, watch for at most T0*EOF periods
        while (AT91C_BASE_TC1->TC_CV < T0 * HITAG_T_WAIT_MAX) {
            // Check if falling edge in tag modulation is detected
            if (AT91C_BASE_TC1->TC_SR & AT91C_TC_LDRAS) {
                // Retrieve the new timing values
                int ra = (AT91C_BASE_TC1->TC_RA / T0);

                // Reset timer every frame, we have to capture the last edge for timing
                AT91C_BASE_TC0->TC_CCR = AT91C_TC_SWTRG;

                LED_B_ON();

                // Capture tag frame (manchester decoding using only falling edges)
                if (ra >= HITAG_T_EOF) {
                    if (rxlen != 0) {
                        //Dbprintf("DEBUG: Wierd1");
                    }
                    // Capture the T0 periods that have passed since last communication or field drop (reset)
                    // We always recieve a 'one' first, which has the falling edge after a half period |-_|
                    response = ra - HITAG_T_TAG_HALF_PERIOD;
                } else if (ra >= HITAG_T_TAG_CAPTURE_FOUR_HALF) {
                    // Manchester coding example |-_|_-|-_| (101)

                    // need to test to verify we don't exceed memory...
                    // if ( ((rxlen+2) / 8) > HITAG_FRAME_LEN) {
                    //     break;
                    // }
                    rx[rxlen / 8] |= 0 << (7 - (rxlen % 8));
                    rxlen++;
                    rx[rxlen / 8] |= 1 << (7 - (rxlen % 8));
                    rxlen++;
                } else if (ra >= HITAG_T_TAG_CAPTURE_THREE_HALF) {
                    // Manchester coding example |_-|...|_-|-_| (0...01)

                    // need to test to verify we don't exceed memory...
                    // if ( ((rxlen+2) / 8) > HITAG_FRAME_LEN) {
                    //     break;
                    // }
                    rx[rxlen / 8] |= 0 << (7 - (rxlen % 8));
                    rxlen++;
                    // We have to skip this half period at start and add the 'one' the second time
                    if (!bSkip) {
                        rx[rxlen / 8] |= 1 << (7 - (rxlen % 8));
                        rxlen++;
                    }
                    lastbit = !lastbit;
                    bSkip = !bSkip;
                } else if (ra >= HITAG_T_TAG_CAPTURE_TWO_HALF) {
                    // Manchester coding example |_-|_-| (00) or |-_|-_| (11)

                    // need to test to verify we don't exceed memory...
                    // if ( ((rxlen+2) / 8) > HITAG_FRAME_LEN) {
                    //     break;
                    // }
                    if (tag_sof) {
                        // Ignore bits that are transmitted during SOF
                        tag_sof--;
                    } else {
                        // bit is same as last bit
                        rx[rxlen / 8] |= lastbit << (7 - (rxlen % 8));
                        rxlen++;
                    }
                } else {
                    // Dbprintf("DEBUG: Wierd2");
                    errorCount++;
                    // Ignore wierd value, is to small to mean anything
                }
            }
            // if we saw over 100 wierd values break it probably isn't hitag...
            if (errorCount > 100) break;

            // We can break this loop if we received the last bit from a frame
            if (AT91C_BASE_TC1->TC_CV > T0 * HITAG_T_EOF) {
                if (rxlen > 0) break;
            }
        }

        // Wait some extra time for flash to be programmed
        if ((rxlen == 0) && (writestate == WRITE_STATE_PROG)) {
            AT91C_BASE_TC0->TC_CCR = AT91C_TC_SWTRG;
            while (AT91C_BASE_TC0->TC_CV < T0 * (HITAG_T_PROG - HITAG_T_WAIT_MAX));
        }
    }

    LEDsoff();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    set_tracing(false);

    AT91C_BASE_TC1->TC_CCR = AT91C_TC_CLKDIS;
    AT91C_BASE_TC0->TC_CCR = AT91C_TC_CLKDIS;

    StartTicks();

    reply_old(CMD_ACK, bSuccessful, 0, 0, (uint8_t *)tag.sectors, 48);
}
