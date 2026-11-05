/* Port version: also defines data_ov107_020cb628, the 8 bytes right before this table, which
 * the decomp leaves undefined although code reads it. wasm-ld resolves an undefined data symbol
 * to address 0 without a word (--allow-undefined), so the code read zeros. The values are the
 * arrays {7, 3, 11, 2} and {1, 2, 4, 8} that Ov107_AiState_ApplyHit (whose .rodata it is) builds
 * from it. Ov107_AiState_OnDefeat and func_ov107_020c73a0 read 4-byte messages [2] and [3] from
 * it, which are this file's table: with zeros there a defeated enemy (the Yellow Opera in the
 * second wave of Mission 02) posted "defeated" to itself until the stack ran out. Both are laid
 * out in DS order, next to each other, as in the ROM. */
/* ov107 .rodata tables, 0x020cb630-0x020cb638.
 *
 * 1 contiguous tables, each written in the width its contents are in:
 * words where the values are small integers, bytes where the words are
 * packed bytes.
 */

#include "nitro/types.h"

const u8 data_ov107_020cb628[8] = {
    7, 3, 11, 2, 1, 2, 4, 8,
};

const u8 data_ov107_020cb630[8] = {
    0, 0, 8, 9, 0, 0, 8, 4,
};
