/* ----------------------------------------------------------------------------
 * Copyright (c) 2013, 2014 Ben Blazak <benblazak.dev@gmail.com>
 * Released under The MIT License (see "doc/licenses/MIT.md")
 * Project located at <https://github.com/benblazak/ergodox-firmware>
 * ------------------------------------------------------------------------- */

/**                                                                 description
 * Implements the eeprom-macro functionality defined in "../eeprom-macro.h" for
 * the ATMega32U4
 *
 *
 * Implementation notes:
 *
 * - The default state (the "erased" state) of this EEPROM is all `1`s, which
 *   makes setting a byte to `0xFF` easier and faster in hardware than zeroing
 *   it (and also causes less wear on the memory over time, I think).  This is
 *   reflected in some of the choices for default values, and such.
 *
 * - GCC and AVR processors (and Intel processors, for that matter) are
 *   primarily little endian: in avr-gcc, multi-byte data types are allocated
 *   with the least significant byte occupying the lowest address.  Protocols,
 *   data formats (including UTF-8), and such are primarily big endian.  I like
 *   little endianness better -- it just feels nicer to me -- but after writing
 *   a bit of code, it seems that big endian serializations are easier to work
 *   with, at least in C.  For that reason, this code organizes bytes in a big
 *   endian manner whenever it has a choice between the two.
 *
 * - For a long time, I was going to try to make this library robust in the
 *   event of power loss, but in the end I decided not to.  This feature is
 *   meant to be used for *temporary* macros - so, with the risk of power loss
 *   during a critical time being fairly low, and the consequence of (detected)
 *   data corruption hopefully more of an annoyance than anything else, I
 *   decided the effort (and extra EEMEM usage) wasn't worth it.
 *
 *
 * TODO:
 * - i was thinking before that the calling function need not ignore layer
 *   shift keys, or any other keys.  now i think that layer keys (or at least
 *   layer shift keys) really should be ignored.  not doing so may lead to all
 *   sorts of fun problems.  for example, if the "begin/end recording" key is
 *   not on layer 0 (which it probably won't be), the last keys pressed (but
 *   not released) will most likely be layer shift keys -- but since these keys
 *   were not released before we stopped recording, there would be no record of
 *   their release, and the macro would therefore push that layer onto the
 *   layer stack, and never pop it off.
 *
 * - 255 bytes (so, on average, about 100 keystrokes = 200 key actions) should
 *   be enough for a macro, i think.  `length` can be 1 byte, and count the
 *   total number of bytes (including `type` and `length`, and anything else)
 *     - also, if the following macro has the same UID, perhaps we should
 *       consider that macro a continuation of the first.
 *
 * - need to write something like:
 *     - `kb__layout__exec_key_layer()`
 *         - `kb__layout__exec_key()` could just look up the current layer
 *           (falling through for transparent keys), and then call
 *           `kb__layout__exec_key_layer()`.  this would obviate the need for a
 *           separate `static get_layer(void)` function, since the
 *           functionality would essentially be separated out anyway.
 *     - `kb__led__delay__error()`
 *         - "delay" because it should probably flash a few times, or
 *           something, and i feel like it'd be better overall to not continue
 *           accepting input while that's happening.
 */


#include <stdint.h>
#include "../../../../firmware/keyboard.h"
#include "../../../../firmware/lib/eeprom.h"
#include "../eeprom-macro.h"

// ----------------------------------------------------------------------------
// checks ---------------------------------------------------------------------

/**                           macros/OPT__EEPROM__EEPROM_MACRO__END/description
 * Implementation notes:
 * - The ATMega32U4 only has 1024 bytes of EEPROM (beginning with byte 0)
 */
#if OPT__EEPROM__EEPROM_MACRO__END > 1023
    #error "OPT__EEPROM__EEPROM_MACRO__END must not be greater than 1023"
#endif

#if OPT__EEPROM__EEPROM_MACRO__END - OPT__EEPROM__EEPROM_MACRO__START < 300
    #warn "Only a small space has been allocated for macros"
#endif

// ----------------------------------------------------------------------------
// macros ---------------------------------------------------------------------

/**                                                  macros/VERSION/description
 * The version number of the EEMEM layout
 *
 * Assignments:
 * - 0x00: Reserved: EEPROM not yet initialized, or in inconsistent state
 * - 0x01: First version
 * - ... : (not yet assigned)
 * - 0xFF: Reserved: EEPROM not yet initialized, or in inconsistent state
 */
#define  VERSION  0x01

/**                                     macros/(group) EEMEM layout/description
 * To define the layout of our section of the EEPROM
 *
 * Members:
 * - `EEMEM_START`: The address of the first byte of our block of EEMEM
 * - `EEMEM_START_ADDRESS_START`
 * - `EEMEM_START_ADDRESS_END`
 * - `EEMEM_END_ADDRESS_START`
 * - `EEMEM_END_ADDRESS_END`
 * - `EEMEM_VERSION_START`
 * - `EEMEM_VERSION_END`
 * - `EEMEM_MACROS_START`
 * - `EEMEM_MACROS_END`
 * - `EEMEM_END`: The address of the last byte of our block of EEMEM
 *
 * Warnings:
 * - This implementation of macros doesn't leave any room for error checking:
 *   we must be very careful not to corrupt the data.  Also need to be very
 *   careful that any pointer into the EEMEM that's supposed to be pointing to
 *   the beginning of a macro (especially a non-initial macro) actually does
 *   point to one.  Otherwise, behavior is undefined.
 *
 * Terms:
 * - The "address" of a macro is the EEMEM address of the first byte of that
 *   macro.
 * - The "header" of a macro is the part of the macro containing the macro's
 *   type and length.
 * - The "data" of a macro is everything following the macro's header.
 *
 * Notes:
 * - `START_ADDRESS` and `END_ADDRESS` are written as part of our effort to
 *   make sure that the assumptions in place when writing the data don't shift
 *   (undetected) by the time it gets read.  Either of these values could
 *   change, legitimately, without `VERSION` being incremented, but it's
 *   important that any two builds of the firmware that deal with this section
 *   of the EEPROM have the same values for each.
 *
 *
 * EEMEM sections:
 *
 * - START_ADDRESS:
 *     - byte 0: MSB of `EEMEM_START`
 *     - byte 1: LSB of `EEMEM_START`
 *
 *     - Upon initialization, if this block does not have the expected value,
 *       our portion of the EEPROM should be reinitialized.
 *
 * - END_ADDRESS:
 *     - byte 0: MSB of `EEMEM_END`
 *     - byte 1: LSB of `EEMEM_END`
 *
 *     - Upon initialization, if this block does not have the expected value,
 *       our portion of the EEPROM should be reinitialized.
 *
 * - VERSION:
 *     - byte 0:
 *         - This byte will be set to `VERSION` as the last step of
 *           initializing our portion of the EEPROM.
 *         - Upon initialization, if this value is not equal to the current
 *           `VERSION`, our portion of the EEPROM should be reinitialized.
 *
 * - MACROS: byte 0..`(EEMEM_END - EEMEM_VERSION_END - 1)`:
 *     - This section will contain a series of zero or more macros, each with
 *       the following format:
 *         - byte 0: `type == TYPE_DELETED`
 *             - byte 1: `length`: the total number of bytes used by this
 *               macro, including the bytes for `type` and `length`
 *             - byte 2...: (optional) undefined
 *         - byte 0: `type == TYPE_VALID_MACRO`
 *             - byte 1: `length`: the total number of bytes used by this
 *               macro, including the bytes for `type` and `length`
 *             - byte 2...: (variable length, as described below)
 *                 - `key-action` 0: the key-action which this macro remaps
 *             - byte ...: (optional) (variable length, as described below)
 *                 - `key-action` 1...: the key-actions to which `key-action` 0
 *                   is remapped
 *         - byte 0: `type == TYPE_CONTINUED`
 *             - byte 1: `length`: the total number of bytes used by this
 *               macro, including the bytes for `type` and `length`
 *             - byte 2...: (optional) a continuation of the data section of
 *               the previous macro
 *         - byte 0: `type == TYPE_END`
 *             - byte 1...: (optional) undefined
 *
 *     - The last macro in this series will have `type == TYPE_END`.
 *
 *     - A key-action is a variable length encoding of the information in a
 *       `key_action_t`, with the following format:
 *
 *           byte 0
 *           .----------------------------------------------.
 *           |     7     |    6    | 5 | 4 | 3 | 2 | 1 | 0  |
 *           |----------------------------------------------|
 *           | continued | pressed | layer |  row  | column |
 *           '----------------------------------------------'
 *
 *           byte 1..3 (optional)
 *           .----------------------------------------------.
 *           |     7     |    6    | 5 | 4 | 3 | 2 | 1 | 0  |
 *           |----------------------------------------------|
 *           | continued |    1    | layer |  row  | column |
 *           '----------------------------------------------'
 *
 *         - `continued`:
 *             - `1`: The next byte is part of this key-action
 *             - `0`: The next byte is not part of this key-action (i.e. this
 *                    is the last byte in this key-action)
 *
 *         - `pressed`:
 *             - This value is stored *only* in the first byte.  In all
 *               subsequent bytes the bit should be set to `1`.
 *
 *         - `layer`, `row`, `column`:
 *             - In the first byte of this key-action, these fields contain the
 *               two most significant bits of their respective values such that
 *               these bits are nonzero in *any* of `layer`, `row`, or
 *               `column`.  In subsequent bytes of this key-action, these
 *               fields contain the pair of bits to the right of the pair of
 *               bits in the previous key-action byte (the next less
 *               significant pair of bits).  If `layer`, `row`, and `column`
 *               all equal `0`, then these three fields will all equal `0`, and
 *               there will only be 1 byte written for this key-action.
 *
 *         - Example of an encoded key-action:
 *
 *               --- as a key_action_t ---
 *               pressed = false
 *               layer   = 0 b 00 00 01 00
 *               row     = 0 b 00 01 10 01
 *               column  = 0 b 00 10 00 11
 *                             |        '- least significant pair of bits
 *                             '- most significant pair of bits
 *
 *               --- in EEMEM ---
 *               byte 0 = 0 b 1 0 00 01 10
 *               byte 1 = 0 b 1 1 01 10 00
 *               byte 2 = 0 b 0 1 00 01 11
 *                            | | |  |  '- column bit pair
 *                            | | |  '- row bit pair
 *                            | | '- layer bit pair
 *                            | '- pressed / 1
 *                            '- continued
 */
#define  EEMEM_START                ((void *)OPT__EEPROM__EEPROM_MACRO__START)
#define  EEMEM_START_ADDRESS_START  (EEMEM_START               + 0)
#define  EEMEM_START_ADDRESS_END    (EEMEM_START_ADDRESS_START + 1)
#define  EEMEM_END_ADDRESS_START    (EEMEM_START_ADDRESS_END + 1)
#define  EEMEM_END_ADDRESS_END      (EEMEM_END_ADDRESS_START + 1)
#define  EEMEM_VERSION_START        (EEMEM_END_ADDRESS_END + 1)
#define  EEMEM_VERSION_END          (EEMEM_VERSION_START   + 0)
#define  EEMEM_MACROS_START         (EEMEM_VERSION_END + 1)
#define  EEMEM_MACROS_END           (EEMEM_END         - 0)
#define  EEMEM_END                  ((void *)OPT__EEPROM__EEPROM_MACRO__END)

/**                                             macros/(group) type/description
 * Aliases for valid values of the "type" field in `MACROS`
 *
 * Members:
 * - `TYPE_DELETED`
 * - `TYPE_VALID_MACRO`
 * - `TYPE_CONTINUED`
 * - `TYPE_END`
 */
#define  TYPE_DELETED      0x00
#define  TYPE_VALID_MACRO  0x01
#define  TYPE_CONTINUED    0x02
#define  TYPE_END          0xFF

// ----------------------------------------------------------------------------
// types ----------------------------------------------------------------------

/**                                              types/key_action_t/description
 * To hold everything needed to represent a single key-action (the press or
 * release of a specific key on a specific layer of the layout matrix).
 *
 * Struct members:
 * - `pressed`: Whether the key is pressed (`true`) or not (`false`)
 * - `layer`: The layer of the key, in the layout matrix
 * - `row`: The row of the key, in the layout matrix
 * - `column`: The column of the key, in the layout matrix
 *
 * Notes:
 * - Since these fields together can reference any key (on any layer)
 *   unambiguously, a `key_action_t` may also serve as a UID for a key.
 */
typedef struct {
    bool    pressed;
    uint8_t layer;
    uint8_t row;
    uint8_t column;
} key_action_t;

// ----------------------------------------------------------------------------
// variables ------------------------------------------------------------------

/**                                             variables/end_macro/description
 * The EEMEM address of the macro with `type == TYPE_END`
 */
void * end_macro;

/**                                         variables/new_end_macro/description
 * The EEMEM address of where to write the next byte of a new macro (or a macro
 * in progress)
 */
void * new_end_macro;

// ----------------------------------------------------------------------------
// local functions ------------------------------------------------------------

/**                                       functions/read_key_action/description
 * Read and return the key-action beginning at `from` in the EEPROM.
 *
 * Arguments:
 * - `from`: A pointer to the location in EEPROM from which to begin reading
 *
 * Returns:
 * - success: The key-action, as a `key_action_t`
 *
 * Notes:
 * - See the documentation for "(group) EEMEM layout" above for a description
 *   of the layout of key-actions in EEMEM.
 */
key_action_t read_key_action(void * from) {
    uint8_t byte;

    // handle the first byte
    // - since this byte (and no others) stores the value of `k.pressed`
    // - also, this allows us to avoid `|=` in favor of `=` for this byte
    
    byte = eeprom__read(from++);

    key_action_t k = {
        .pressed = byte >> 6 & 0b01,
        .layer   = byte >> 4 & 0b11,
        .row     = byte >> 2 & 0b11,
        .column  = byte >> 0 & 0b11,
    };

    // handle all subsequent bytes
    // - we assume the stream is valid.  especially, we do not check to make
    //   sure that the key-action is no more than 4 bytes long.

    while (byte >> 7) {
        byte = eeprom__read(from++);

        // shift up (make more significant) the bits we have so far, to make
        // room for the bits we just read
        k.layer  <<= 2;
        k.row    <<= 2;
        k.column <<= 2;

        // logical or the bits we just read into the lowest (least significant)
        // positions
        k.layer  |= byte >> 4 & 0b11;
        k.row    |= byte >> 2 & 0b11;
        k.column |= byte >> 0 & 0b11;
    }

    return k;  // success
}

/**                                      functions/write_key_action/description
 * Write the given information to a key-action beginning at `to` in the
 * EEPROM, and return the number of bytes written.
 *
 * Arguments:
 * - `to`: A pointer to the location in EEPROM at which to begin writing
 * - `k`: The key-action to write
 *
 * Returns:
 * - success: The number of bytes written
 * - failure: 0
 *
 * Notes:
 * - See the documentation for "(group) EEMEM layout" above for a description
 *   of the layout of key-actions in EEMEM.
 *
 * Implementation notes:
 * - We handle the `layer`, `row`, and `column` variables (inside `k`) as being
 *   made up of 4 pairs of bits.
 * - We deal with these bits beginning with the high (most significant) pair,
 *   and shifting left (towards the most significant end of the byte) to
 *   discard bit pairs we're done with.
 *     - This method seemed faster (i.e. generated less assembly code) when I
 *       was testing than leaving the `layer`, `row`, and `column` bytes as
 *       they were and using a variable mask (as in `k.layer & 0b11 << i*2`).
 *       It's probably worthwhile to note that I was looking at the assembly
 *       (though not closely) and function size with optimizations turned on.
 */
uint8_t write_key_action(void * to, key_action_t k) {

    // - we need to leave room after this macro (and therefore after this
    //   key-action) for the `type == TYPE_END` byte
    if (to > EEMEM_END-4)
        return 0;  // error: might not be enough space

    // ignore the bits we don't need to write
    // - if the leading two bits of all three variables are `0b00`, we don't
    //   need to write a key-action byte containing that pair of bits
    // - the maximum number of pairs of bits we can ignore is 3; the last pair
    //   (the least significant) must be written to the EEPROM regardless of
    //   its value
    // - we set `i` here (and make it global to the function) because we need
    //   to make sure to *consider writing* exactly 4 pairs of bits.  some may
    //   be skipped, some or all may be written, but the total of both must be
    //   4.

    uint8_t i = 0;

    for (; i<3 && !((k.layer|k.row|k.column) & 0xC0); i++) {
        k.layer  <<= 2;
        k.row    <<= 2;
        k.column <<= 2;
    }

    uint8_t written = 4-i;

    // write key-action bytes for all bit pairs that weren't ignored
    // - the first byte contains the value of `k.pressed`; the same position is
    //   set to `1` in all subsequent bytes
    // - all bytes except the last one written (containing the least
    //   significant bits) have their first bit set to `1`

    uint8_t byte = k.pressed << 6;

    for (; i<4; i++) {
        byte = byte | ( i<3             ) << 7
                    | ( k.layer  & 0xC0 ) >> 2
                    | ( k.row    & 0xC0 ) >> 4
                    | ( k.column & 0xC0 ) >> 6 ;
        eeprom__write(to++, byte);
        byte = 1 << 6;

        k.layer  <<= 2;
        k.row    <<= 2;
        k.column <<= 2;
    }

    return written;  // success
}

/**                                       functions/find_key_action/description
 * Find the macro remapping the given key-action (if it exists).
 *
 * Arguments:
 * - `k`: The key-action to search for
 *
 * Returns:
 * - success: The EEMEM address of the desired macro
 * - failure: `0`
 *
 * Notes:
 * - The address `0` (or really `NULL`, which is `#define`ed to `((void *)0)`)
 *   is a valid address in the EEPROM; but because macros are not placed first
 *   in the EEPROM, we can still use it to signal nonexistence or failure.
 * - See the documentation for "(group) EEMEM layout" above for a description
 *   of the layout of macros in EEMEM.
 *
 * Implementation notes:
 * - It would be more efficient to convert the given key action into the same
 *   binary representation as used in the EEPROM, once, and then compare that
 *   directly with the encoded key-action bytes read; but I don't think it'll
 *   have enough of an impact on performance to justify rewriting the
 *   ...key_action() functions, and it seems like this solution is a little bit
 *   cleaner (since it results in slightly fewer functions and keeps the
 *   representation of a key-function in SRAM consistent).
 */
void * find_key_action(key_action_t k) {
    void * current = EEMEM_MACROS_START;

    for ( uint8_t type = eeprom__read(current);
          type != TYPE_END;
          current += eeprom__read(current+1), type = eeprom__read(current) ) {

        if (type == TYPE_VALID_MACRO) {

            key_action_t k_current = read_key_action(current+2);

            if (    k.pressed == k_current.pressed
                 && k.layer   == k_current.layer
                 && k.row     == k_current.row
                 && k.column  == k_current.column ) {

                return current;
            }
        }
    }

    return 0;  // key-action not found
}

/**                                     functions/find_next_deleted/description
 * Find the first deleted macro at or after the given macro.
 *
 * Arguments:
 * - `start`: The EEMEM address of the macro at which to begin searching
 *
 * Returns:
 * - success: The EEMEM address of the first deleted macro at or after `start`
 * - failure: `0` (no deleted macros were found at or after `start`)
 */
void * find_next_deleted(void * start) {
    for ( uint8_t type = eeprom__read(start);
          type != TYPE_END;
          start += eeprom__read(start+1), type = eeprom__read(start) ) {

        if (type == TYPE_DELETED)
            return start;
    }

    return 0;  // no deleted macro found
}

/**                                  functions/find_next_nondeleted/description
 * Find the first macro at or after the given macro that is not marked as
 * deleted.
 *
 * Arguments:
 * - `start`: The EEMEM address of the macro at which to begin searching
 *
 * Returns:
 * - success: The EEMEM address of the first non-deleted macro at or after
 *   `start`
 *
 * Notes:
 * - Since the sequence of macros must end with a `TYPE_END` macro (which is,
 *   of course, not a deleted macro), this function will always find a
 *   non-deleted macro at or after the one passed.
 */
void * find_next_nondeleted(void * start) {
    for ( uint8_t type = eeprom__read(start);
          type == TYPE_DELETED || type == TYPE_CONTINUED;
          start += eeprom__read(start+1), type = eeprom__read(start) );

    return start;
}

/**                                              functions/compress/description
 * TODO
 *
 * - it might be possible to let in-progress macros keep being written, even
 *   when a compress() gets called, transparently.  since writes to the eeprom
 *   (using my wrapper) are scheduled and sequential, all we would have to do
 *   would be to make sure to copy the in-progress bytes, and adjust the
 *   necessary variables so future writes to the in-progress macro would be
 *   scheduled to occur in the appropriate location (and also so that the final
 *   write validating the macro would occur in the correct location).  then, we
 *   would only be bound by memory (for scheduling writes), and by the total
 *   amount of unused EEPROM space for macros.  we would still be vulnerable to
 *   power loss though... but handling that cleanly would be too much trouble.
 *
 * - do we clear the `VERSION` byte?  maybe not... :)
 *         - This byte will be cleared (to 0xFF) before beginning a
 *           `compress()` of the macros, and reset to `VERSION` once the
 *           operation has completed.
 *
 * - another advantage of not clearing the version byte is that we can search
 *   for and play back macros as usual; if we're in the middle of compressing,
 *   and the macro hasn't been dealt with yet, it will simply appear not to
 *   exist for a few seconds.
 */
void compress(void) {  // TODO

    // `to_overwrite` is the first byte of the EEPROM with a value we don't
    // care about
    // - this will only point to the beginning of a macro initially
    void * to_overwrite = find_next_deleted(EEMEM_MACROS_START);
    if (! to_overwrite)
        return;

    // set `next` to a value that works when we enter the loop
    // - on the first iteration, `find_next_nondeleted(next)` will return
    //   quickly, so this doesn't waste much time
    // - we should do this before writing the `TYPE_END` byte to the EEPROM
    //   below.  since writes are delayed until the end of the keyboard scan
    //   cycle (which can't happen until sometime after this function returns),
    //   it doesn't really matter -- we could just set `next = to_overwrite` --
    //   but it's nice to write things so they would work even if writes were
    //   not delayed.
    void * next = find_next_nondeleted(to_overwrite);

    eeprom__write(to_overwrite, TYPE_END);

    while (next != new_end_macro) {

        // `to_compress` is the beginning of the data we wish to copy
        void * to_compress = find_next_nondeleted(next);

        // `next` will be 1 byte beyond the data we wish to copy
        next = find_next_deleted(to_compress);
        if (! next)
            next = new_end_macro;

        uint8_t type = eeprom__read(to_compress);
        void * type_location = to_overwrite;
        to_overwrite++;
        to_compress++;

        for ( uint16_t length = next-to_compress;
              length;
              length = next-to_compress ) {

            if (length > UINT8_MAX)
                length = UINT8_MAX;

            eeprom__copy(to_overwrite, to_compress, length);
            to_overwrite += length;
            to_compress += length;
        }

        if (next != new_end_macro)
            eeprom__write(to_overwrite, TYPE_END);

        eeprom__write(type_location, type);
    }
}

// ----------------------------------------------------------------------------
// public functions -----------------------------------------------------------

uint8_t eeprom_macro__init(void) {
    // TODO
    return 0;
}

uint8_t eeprom_macro__record_init( bool    pressed,
                                   uint8_t layer,
                                   uint8_t row,
                                   uint8_t column ) {
    // TODO
    return 0;
}

uint8_t eeprom_macro__record_action( bool    pressed,
                                     uint8_t layer,
                                     uint8_t row,
                                     uint8_t column ) {
    // TODO
    return 0;
}

uint8_t eeprom_macro__record_finalize(void) {
    // TODO
    return 0;
}

uint8_t eeprom_macro__play( bool    pressed,
                            uint8_t layer,
                            uint8_t row,
                            uint8_t column ) {
    // TODO
    return 0;
}

bool eeprom_macro__exists( bool    pressed,
                           uint8_t layer,
                           uint8_t row,
                           uint8_t column ) {
    // TODO
    return false;
}

void eeprom_macro__clear( bool    pressed,
                          uint8_t layer,
                          uint8_t row,
                          uint8_t column ) {
    // TODO
}

void eeprom_macro__clear_all(void) {
    // TODO
}

