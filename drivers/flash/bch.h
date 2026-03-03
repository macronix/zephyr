/*
 * Copyright (c) 2022-2025 Macronix International Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _BCH_H
#define _BCH_H

#include <stdint.h>
#include <stdbool.h>
#define ROUNDUP_DIV(_val, _base) (((_base)-1 + (_val)) / (_base))

typedef struct {
 int      m;
 int      t;
 int      n;
 int      ecc_bits;
 int      ecc_bytes;
 int      ecc_words;
 uint32_t size_step;
 uint32_t len;
 uint8_t  le;

 uint16_t *a_pow;
 uint16_t *a_log;

 uint32_t *mod_tab;
 uint32_t *ecc;
 uint32_t *syn;
 uint32_t *elp;
 uint32_t *buf;
 uint32_t *buf2;
 uint32_t *g;
} bch_t;

/**
 * bch_init - initialize BCH codec
 * @m:         GF Galois Field parameter
 * @t:         Error Correction Capability
 * @size_step: NUmber of data bytes per encoding step
 * @bch_ret:   output，points to the allocated bch_t on success
 *
 * return 0 on success，or error code
 */
int bch_init(int m, int t, uint32_t size_step, bch_t **bch_ret);

/**
 * bch_encode - Perform BCH encoding
 * @bch:  BCH control structure
 * @data: input data（size_step bytes）
 * @ecc:  output ECC（ecc_bytes bytes），if NULL only updates internal ecc
 */
void bch_encode(bch_t *bch, uint8_t *data, uint8_t *ecc);

/**
 * bch_decode - Perform BCH decoding and error correction
 * @bch:  BCH control structure
 * @data: input/output data（size_step bytes，error bits are corrected in place）
 * @ecc:  Stored ECC（ecc_bytes bytes）
 *
 * return the number of corrected bits
 *   0 if no errors were found，
 *   -1 if errors were not correctable
 */
int bch_decode(bch_t *bch, uint8_t *data, uint8_t *ecc);

/**
 * bch_free - Release BCH control structure
 * @bch:  BCH control structure
 */
void bch_free(bch_t *bch);
#endif
