/*
 * Copyright (c) 2022-2025 Macronix International Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bch.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifndef CONFIG_SPI_NAND_BCH_HEAP_SIZE
#error "CONFIG_SPI_NAND_BCH_HEAP_SIZE is not defined"
#endif

#define BCH_MAX_M          13
#define BCH_MAX_T          12
#define MAX_GEN_POLY_SIZE  (BCH_MAX_M * BCH_MAX_T + 2)   /* 158 */
#define BCH_BUF3_MAX_WORDS  (((1 << BCH_MAX_M) / 8 / 4) + BCH_MAX_T)  /* 268 */

K_HEAP_DEFINE(bch_heap, CONFIG_SPI_NAND_BCH_HEAP_SIZE);

LOG_MODULE_REGISTER(bch, CONFIG_FLASH_LOG_LEVEL);

static void *bch_alloc(size_t size, int *err)
{
	void *ptr = NULL;

	if (*err == 0) {
		ptr = k_heap_alloc(&bch_heap, size, K_NO_WAIT);
		if (ptr != NULL) {
			(void)memset(ptr, 0, size);
		}
	}
	if (ptr == NULL) {
		*err = 1;
	}
	return ptr;
}

#if defined(CONFIG_SPI_NAND_BCH_LUT_STATIC)
static const uint8_t pow_m13[] = {
#include "pow.inc"
};

static const uint8_t log_m13[] = {
#include "log.inc"
};
#endif

static inline uint32_t swap32_byte(uint32_t val)
{
    return (val & 0xff000000) >> 24 |
           (val & 0x00ff0000) >> 8  |
           (val & 0x0000ff00) << 8  |
           (val & 0x000000ff) << 24;
}

static inline int mod_n(bch_t *bch, uint32_t v)
{
    if (v < (2 * bch->n)) {
        return (v >= bch->n) ? (v - bch->n) : v;
    }

    while (v >= bch->n) {
        v -= bch->n;
        v = (v & bch->n) + (v >> bch->m);
    }
    return v;
}

static void build_syndrome(bch_t *bch)
{
    int i, j;
    int ecc_bits = bch->ecc_bits;
    uint32_t *ecc = bch->ecc;

    memset(bch->syn, 0, 2 * bch->t * sizeof(*bch->syn));

    while (ecc_bits > 0) {
        i = ecc_bits - 32;
        ecc_bits = i;
        while (*ecc > 0) {
            if ((*ecc & 1) != 0U) {
                for (j = 0; j < 2 * bch->t; j++) {
                    bch->syn[j] ^= bch->a_pow[mod_n(bch, (j + 1) * i)];
                }
            }
            *ecc >>= 1;
            i++;
        }
        ecc++;
    }
}

static int build_error_location_poly(bch_t *bch)
{
    int i, j, k;
    int deg, buf_deg, tmp_deg;
    int pp = -1;
    uint32_t tmp, dp = 1, d = bch->syn[0];

    memset(bch->elp, 0, (bch->t + 1) * sizeof(*bch->elp));
    buf_deg = 0;
    bch->buf[0] = 1;
    deg = 0;
    bch->elp[0] = 1;

    for (i = 0; (i < bch->t) && (deg <= bch->t); i++) {
        if (d != 0) {
            k = 2 * i - pp;
            if (buf_deg + k > deg) {
                tmp_deg = deg;
                for (j = 0; j <= deg; j++) {
                    bch->buf2[j] = bch->elp[j];
                }
            }
            tmp = bch->n + bch->a_log[d] - bch->a_log[dp];
            for (j = 0; j <= buf_deg; j++) {
                if (bch->buf[j] != 0) {
                    bch->elp[j + k] ^=
                        bch->a_pow[mod_n(bch, tmp + bch->a_log[bch->buf[j]])];
                }
            }
            if (buf_deg + k > deg) {
                deg = buf_deg + k;
                buf_deg = tmp_deg;
                for (j = 0; j <= tmp_deg; j++) {
                    bch->buf[j] = bch->buf2[j];
                }
                dp = d;
                pp = 2 * i;
            }
        }
        if (i < bch->t - 1) {
            k = 2 * i + 1;
            d = bch->syn[k + 1];
            for (j = 1; j <= deg; j++) {
                if (bch->elp[j] && bch->syn[k]) {
                    d ^= bch->a_pow[mod_n(bch,
                        bch->a_log[bch->elp[j]] + bch->a_log[bch->syn[k]])];
                }
                k--;
            }
        }
    }
    return (deg > bch->t) ? -1 : deg;
}

static int chien_search(bch_t *bch, int deg)
{
    int i, j, nroot = 0;
    int *root = (int *)bch->buf2;
    uint32_t syn, syn0;
    int reg[BCH_MAX_T + 1];
    int step[BCH_MAX_T + 1];
    int k = bch->n - bch->a_log[bch->elp[deg]];

    for (j = 0; j < deg; j++) {
        if (bch->elp[j]) {
            reg[j]  = mod_n(bch, bch->a_log[bch->elp[j]] + k);
            step[j] = j;
        } else {
            reg[j]  = -1;
            step[j] = 0;
        }
    }
    reg[deg]  = 0;
    step[deg] = deg;

    syn0 = bch->elp[0] ? bch->a_pow[reg[0]] : 0;

    for (i = 0; i <= bch->n; i++) {
        for (j = 1, syn = syn0; j <= deg; j++) {
            if (reg[j] >= 0) {
                syn ^= bch->a_pow[reg[j]];
            }
        }
        if (syn == 0) {
            root[nroot++] = bch->n - i;
            if (nroot == deg) {
                return nroot;
            }
        }

        for (j = 1; j <= deg; j++) {
            if (reg[j] >= 0) {
                reg[j] += step[j];
                if (reg[j] >= bch->n) {
                    reg[j] -= bch->n;
                }
            }
        }
    }
    return 0;
}

static void build_gf_table(bch_t *bch)
{
    uint32_t x, msb, poly;
    uint32_t prim_poly[] = {0x11d, 0x211, 0x409, 0x805, 0x1053, 0x201b};

    poly = prim_poly[bch->m - 8];
    msb  = 1 << bch->m;

    bch->a_pow[0] = 1;
    bch->a_log[1] = 0;
    x = 2;

    for (int i = 1; i < bch->n; i++) {
        bch->a_pow[i] = (uint16_t)x;
        bch->a_log[x] = (uint16_t)i;
        x <<= 1;
        if (x >= msb) {
            x ^= poly;
        }
    }

    bch->a_pow[bch->n] = 1;
    bch->a_log[0]      = 0;
}

static void build_mod_tables(bch_t *bch, const uint32_t *g)
{
    int i, j, b, d;
    int plen   = (bch->ecc_bits + 32) / 32;
    int ecclen = (bch->ecc_bits + 31) / 32;
    uint32_t data, hi, lo, *tab, poly;

    for (i = 0; i < 4; i++) {
        for (b = 0; b < 16; b++) {
            tab  = bch->mod_tab + (b * 4 + i) * bch->ecc_words;
            data = i << (2 * b);
            while (data) {
                d    = 0;
                poly = (data >> 1);
                while (poly) {
                    poly >>= 1;
                    d++;
                }
                data ^= g[0] >> (31 - d);
                for (j = 0; j < ecclen; j++) {
                    hi = (d < 31) ? g[j] << (d + 1) : 0;
                    lo = (j + 1 < plen) ? g[j + 1] >> (31 - d) : 0;
                    tab[j] ^= hi | lo;
                }
            }
        }
    }
}

static void build_generator_poly(bch_t *bch)
{
    int i, j, k, m, t;
    uint32_t n;
    uint32_t x[MAX_GEN_POLY_SIZE] = {0};

    for (t = 0, x[0] = 1, bch->ecc_bits = 0; t < bch->t; t++) {
        for (m = 0, i = 2 * t + 1; m < bch->m; m++) {
            x[bch->ecc_bits + 1] = 1;
            for (j = bch->ecc_bits; j > 0; j--) {
                if (x[j] != 0) {
                    x[j] = bch->a_pow[mod_n(bch, bch->a_log[x[j]] + i)] ^
                           x[j - 1];
                } else {
                    x[j] = x[j - 1];
                }
            }
            if (x[j]) {
                x[j] = bch->a_pow[mod_n(bch, bch->a_log[x[j]] + i)];
            }
            bch->ecc_bits++;
            i = mod_n(bch, 2 * i);
        }
    }

    for (k = bch->ecc_bits + 1, i = 0; k > 0; k = k - n) {
        n = (k > 32) ? 32 : k;
        for (j = 0; j < (int)n; j++) {
            if (x[k - 1 - j] != 0) {
                bch->g[i] |= 1U << (31 - j);
            }
        }
        i++;
    }
}

void bch_encode(bch_t *bch, uint8_t *data, uint8_t *ecc)
{
 int i, j, mlen;
    uint32_t w, ecc_words = bch->ecc_words, *p, *c[16], *t[16], *mod_tab_base = bch->mod_tab;
    uint32_t buf3[BCH_BUF3_MAX_WORDS] = {};

    memset(bch->ecc, 0, ecc_words * sizeof(*bch->ecc));
    memcpy((uint8_t *)buf3 + bch->ecc_words * sizeof(uint32_t), data, bch->size_step);

    for (i = 0; i < 16; i++) {
     t[i] = mod_tab_base + (i * 4 * ecc_words);
    }
    p    = buf3;
    mlen = bch->len / 4;

    while (mlen-- > 0) {
        w = bch->le ? swap32_byte(*p) : *p;
        p++;
        w ^= bch->ecc[0];

        for (i = 0; i < 16; i++) {
         c[i] = t[i] + ecc_words * ((w >> (i * 2)) & 0x03);
        }

        for (i = 0; i < ecc_words - 1;  i++) {
         uint32_t acc = c[0][i];
         for (j = 1; j < 16; j++) {
          acc ^= c[j][i];
         }
         bch->ecc[i] = bch->ecc[i + 1] ^ acc;
        }

        uint32_t acc = c[0][i];
        for (j = 1; j < 16; j++) {
         acc ^= c[j][i];
        }
        bch->ecc[i] = acc;
    }

    if (ecc != NULL) {
        uint8_t tmp[BCH_MAX_M * BCH_MAX_T / 8 + 4] = {};

        for (i = 0; i < ecc_words; i++) {
            uint32_t sw = swap32_byte(bch->ecc[i]);
            memcpy(tmp + i * sizeof(uint32_t), &sw, sizeof(uint32_t));
        }
        memcpy(ecc, tmp, bch->ecc_bytes);
    }
}

int bch_decode(bch_t *bch, uint8_t *data, uint8_t *ecc)
{
    int i, err = 0, nroot;
    int *root = (int *)bch->buf2;
    uint32_t nbits;

    bch_encode(bch, data, NULL);

    uint8_t ecc_buf[BCH_MAX_M * BCH_MAX_T / 8 + 4];
    memset(ecc_buf, 0, bch->ecc_words * sizeof(uint32_t));
    memcpy(ecc_buf, ecc, bch->ecc_bytes);

    for (i = 0; i < bch->ecc_words; i++) {
        uint32_t ecc_in;
        memcpy(&ecc_in, ecc_buf + i * sizeof(uint32_t), sizeof(uint32_t));
        ecc_in = swap32_byte(ecc_in);
        bch->ecc[i] ^= ecc_in;
        err |= bch->ecc[i];
    }

    if (err == 0) {
        return 0;
    }

    build_syndrome(bch);
    err = build_error_location_poly(bch);
    if (err <= 0) {
        return -1;
    }
    nroot = chien_search(bch, err);
    if (err != nroot) {
        return -1;
    }

    nbits = (bch->len * 8) + bch->ecc_bits;
    for (i = 0; i < err; i++) {
        root[i] = nbits - 1 - root[i];
        root[i] = (root[i] & ~7) | (7 - (root[i] & 7));

        if ((root[i] / 8) < (bch->ecc_words * 4)) {
            printf("Error bit is in ecc range form byte 0 to %d, SKIP!!\r\n",
                root[i] / 8);
            continue;
        }
        printf("Before correct: <%d> %02X\r\n",
            (root[i] / 8) - (bch->ecc_words * 4),
            data[(root[i] / 8) - (bch->ecc_words * 4)]);

        data[(root[i] / 8) - (bch->ecc_words * 4)] ^= 1 << (root[i] % 8);

        printf("After  correct: <%d> %02X\r\n",
            (root[i] / 8) - (bch->ecc_words * 4),
            data[(root[i] / 8) - (bch->ecc_words * 4)]);
    }

    return err;
}

int bch_init(int m, int t, uint32_t size_step, bch_t **bch_ret)
{
    bch_t *bch;
    uint32_t a = 1;
	int err = 0;
    uint8_t *p = (uint8_t *)&a;
    *bch_ret = 0;

    if ((m < 8) || (m > BCH_MAX_M) || (t < 1) || (t > BCH_MAX_T)) {
        printf("bch init failed, params should be m: 8~%d, t: 1~%d\r\n",
            BCH_MAX_M, BCH_MAX_T);
        return -1;
    }

	bch = bch_alloc(sizeof(bch_t), &err);

    bch->le        = (*p == 1);
    bch->size_step = size_step;
    bch->m         = m;
    bch->t         = t;
    bch->n         = (1 << m) - 1;
    bch->ecc_words = (m * t + 31) / 32;
    bch->len       = (bch->n + 1) / 8;

    printf("This system is %s endian\r\n", bch->le ? "Little" : "Big");

#if defined(CONFIG_SPI_NAND_BCH_LUT_STATIC)
    bch->a_pow =  (uint16_t *)pow_m13;
    bch->a_log =  (uint16_t *)log_m13;
#else
	bch->a_pow = bch_alloc((bch->n + 1) * sizeof(*bch->a_pow), &err);
	if (err != 0) {
		bch_free(bch);
		return -ENOMEM;
	}

	bch->a_log = bch_alloc((bch->n + 1) * sizeof(*bch->a_log), &err);
	if (err != 0) {
		bch_free(bch);
		return -ENOMEM;
	}
#endif

	bch->mod_tab = bch_alloc(bch->ecc_words * 16 * 4 * sizeof(*bch->mod_tab), &err);
	bch->ecc = bch_alloc(bch->ecc_words * sizeof(*bch->ecc), &err);
	bch->buf = bch_alloc((t + 1) * sizeof(*bch->buf), &err);
	bch->buf2 = bch_alloc((t + 1) * sizeof(*bch->buf2), &err);
	bch->syn = bch_alloc(t * 2 * sizeof(*bch->syn), &err);
	bch->elp = bch_alloc((t + 1) * sizeof(*bch->elp), &err);
	bch->g = bch_alloc((bch->ecc_words + 1) * sizeof(*bch->g), &err);

	if (err != 0) {
		bch_free(bch);
		return -ENOMEM;
	}

#if !defined(CONFIG_SPI_NAND_BCH_LUT_STATIC)
    build_gf_table(bch);
#endif
    build_generator_poly(bch);

    bch->ecc_bytes = (bch->ecc_bits + 7) / 8;
    build_mod_tables(bch, bch->g);

    *bch_ret = bch;
    return 0;
}

void bch_free(bch_t *bch)
{
	if (bch != NULL) {
#if !defined(CONFIG_SPI_NAND_BCH_LUT_STATIC)
		k_heap_free(&bch_heap, bch->a_pow);
		k_heap_free(&bch_heap, bch->a_log);
#endif
		k_heap_free(&bch_heap, bch->mod_tab);
		k_heap_free(&bch_heap, bch->ecc);
		k_heap_free(&bch_heap, bch->syn);
		k_heap_free(&bch_heap, bch->elp);
		k_heap_free(&bch_heap, bch->buf);
		k_heap_free(&bch_heap, bch->buf2);
		k_heap_free(&bch_heap, bch->g);
		k_heap_free(&bch_heap, bch);
	}
}
