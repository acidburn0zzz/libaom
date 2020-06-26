/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <math.h>

#include "config/aom_dsp_rtcd.h"

#include "aom_dsp/quantize.h"
#include "aom_mem/aom_mem.h"
#include "aom_ports/mem.h"

#include "av1/common/idct.h"
#include "av1/common/quant_common.h"
#include "av1/common/scan.h"
#include "av1/common/seg_common.h"

#include "av1/encoder/av1_quantize.h"
#include "av1/encoder/encoder.h"
#include "av1/encoder/rd.h"

void av1_quantize_skip(intptr_t n_coeffs, tran_low_t *qcoeff_ptr,
                       tran_low_t *dqcoeff_ptr, uint16_t *eob_ptr) {
  memset(qcoeff_ptr, 0, n_coeffs * sizeof(*qcoeff_ptr));
  memset(dqcoeff_ptr, 0, n_coeffs * sizeof(*dqcoeff_ptr));
  *eob_ptr = 0;
}

#if CONFIG_EXTQUANT
static void quantize_fp_helper_c(
    const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int32_t *zbin_ptr,
    const int32_t *round_ptr, const int32_t *quant_ptr,
    const int32_t *quant_shift_ptr, tran_low_t *qcoeff_ptr,
    tran_low_t *dqcoeff_ptr, const int32_t *dequant_ptr, uint16_t *eob_ptr,
    const int16_t *scan, const int16_t *iscan, const qm_val_t *qm_ptr,
    const qm_val_t *iqm_ptr, int log_scale) {
#else
static void quantize_fp_helper_c(
    const tran_low_t *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr,
    const int16_t *round_ptr, const int16_t *quant_ptr,
    const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr,
    tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr,
    const int16_t *scan, const int16_t *iscan, const qm_val_t *qm_ptr,
    const qm_val_t *iqm_ptr, int log_scale) {
#endif
  int i, eob = -1;
  const int rounding[2] = { ROUND_POWER_OF_TWO(round_ptr[0], log_scale),
                            ROUND_POWER_OF_TWO(round_ptr[1], log_scale) };
  // TODO(jingning) Decide the need of these arguments after the
  // quantization process is completed.
  (void)zbin_ptr;
  (void)quant_shift_ptr;
  (void)iscan;

  memset(qcoeff_ptr, 0, n_coeffs * sizeof(*qcoeff_ptr));
  memset(dqcoeff_ptr, 0, n_coeffs * sizeof(*dqcoeff_ptr));

  if (qm_ptr == NULL && iqm_ptr == NULL) {
    for (i = 0; i < n_coeffs; i++) {
      const int rc = scan[i];
      const int32_t thresh =
          (int32_t)ROUND_POWER_OF_TWO(dequant_ptr[rc != 0], QUANT_TABLE_BITS);
      const int coeff = coeff_ptr[rc];
      const int coeff_sign = (coeff >> 31);
      int64_t abs_coeff = (coeff ^ coeff_sign) - coeff_sign;
      int tmp32 = 0;
      if ((abs_coeff << (1 + log_scale)) >= thresh) {
        abs_coeff =
            clamp64(abs_coeff + rounding[rc != 0], INT16_MIN, INT16_MAX);
        tmp32 = (int)((abs_coeff * quant_ptr[rc != 0]) >> (16 - log_scale));
        if (tmp32) {
          qcoeff_ptr[rc] = (tmp32 ^ coeff_sign) - coeff_sign;
#if QUANT_TABLE_BITS
          const tran_low_t abs_dqcoeff =
              (tran_low_t)ROUND_POWER_OF_TWO_64(
                  (tran_high_t)tmp32 * dequant_ptr[rc != 0],
                  QUANT_TABLE_BITS) >>
              log_scale;
#else
          const tran_low_t abs_dqcoeff =
              (tmp32 * dequant_ptr[rc != 0]) >> log_scale;
#endif  // QUANT_TABLE_BITS
          dqcoeff_ptr[rc] = (abs_dqcoeff ^ coeff_sign) - coeff_sign;
        }
      }
      if (tmp32) eob = i;
    }
  } else {
    // Quantization pass: All coefficients with index >= zero_flag are
    // skippable. Note: zero_flag can be zero.
    for (i = 0; i < n_coeffs; i++) {
      const int rc = scan[i];
      const int coeff = coeff_ptr[rc];
      const qm_val_t wt = qm_ptr ? qm_ptr[rc] : (1 << AOM_QM_BITS);
      const qm_val_t iwt = iqm_ptr ? iqm_ptr[rc] : (1 << AOM_QM_BITS);
      const int dequant =
          (dequant_ptr[rc != 0] * iwt + (1 << (AOM_QM_BITS - 1))) >>
          AOM_QM_BITS;
      const int coeff_sign = (coeff >> 31);
      int64_t abs_coeff = (coeff ^ coeff_sign) - coeff_sign;
      int tmp32 = 0;
      if ((((tran_high_t)abs_coeff * wt) << QUANT_TABLE_BITS) >=
          ((tran_high_t)dequant_ptr[rc != 0]
           << (AOM_QM_BITS - (1 + log_scale)))) {
        abs_coeff += rounding[rc != 0];
        abs_coeff = clamp64(abs_coeff, INT16_MIN, INT16_MAX);
        tmp32 = (int)((abs_coeff * wt * quant_ptr[rc != 0]) >>
                      (16 - log_scale + AOM_QM_BITS));
        qcoeff_ptr[rc] = (tmp32 ^ coeff_sign) - coeff_sign;
#if QUANT_TABLE_BITS
        const tran_low_t abs_dqcoeff =
            (tran_low_t)ROUND_POWER_OF_TWO_64((tran_high_t)tmp32 * dequant,
                                              QUANT_TABLE_BITS) >>
            log_scale;
#else
        const tran_low_t abs_dqcoeff = (tmp32 * dequant) >> log_scale;
#endif
        dqcoeff_ptr[rc] = (abs_dqcoeff ^ coeff_sign) - coeff_sign;
      }

      if (tmp32) eob = i;
    }
  }
  *eob_ptr = eob + 1;
}

#if CONFIG_EXTQUANT
static void highbd_quantize_fp_helper_c(
    const tran_low_t *coeff_ptr, intptr_t count, const int32_t *zbin_ptr,
    const int32_t *round_ptr, const int32_t *quant_ptr,
    const int32_t *quant_shift_ptr, tran_low_t *qcoeff_ptr,
    tran_low_t *dqcoeff_ptr, const int32_t *dequant_ptr, uint16_t *eob_ptr,
    const int16_t *scan, const int16_t *iscan, const qm_val_t *qm_ptr,
    const qm_val_t *iqm_ptr, int log_scale) {
#else
static void highbd_quantize_fp_helper_c(
    const tran_low_t *coeff_ptr, intptr_t count, const int16_t *zbin_ptr,
    const int16_t *round_ptr, const int16_t *quant_ptr,
    const int16_t *quant_shift_ptr, tran_low_t *qcoeff_ptr,
    tran_low_t *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr,
    const int16_t *scan, const int16_t *iscan, const qm_val_t *qm_ptr,
    const qm_val_t *iqm_ptr, int log_scale) {
#endif
  int i;
  int eob = -1;
  const int shift = 16 - log_scale;
  // TODO(jingning) Decide the need of these arguments after the
  // quantization process is completed.
  (void)zbin_ptr;
  (void)quant_shift_ptr;
  (void)iscan;

  if (qm_ptr || iqm_ptr) {
    // Quantization pass: All coefficients with index >= zero_flag are
    // skippable. Note: zero_flag can be zero.
    for (i = 0; i < count; i++) {
      const int rc = scan[i];
      const int coeff = coeff_ptr[rc];
      const qm_val_t wt = qm_ptr != NULL ? qm_ptr[rc] : (1 << AOM_QM_BITS);
      const qm_val_t iwt = iqm_ptr != NULL ? iqm_ptr[rc] : (1 << AOM_QM_BITS);
      const int dequant =
          (dequant_ptr[rc != 0] * iwt + (1 << (AOM_QM_BITS - 1))) >>
          AOM_QM_BITS;
      const int coeff_sign = (coeff >> 31);
      const int64_t abs_coeff = (coeff ^ coeff_sign) - coeff_sign;
      int abs_qcoeff = 0;
      if ((((tran_high_t)abs_coeff * wt) << QUANT_TABLE_BITS) >=
          ((tran_high_t)dequant_ptr[rc != 0]
           << (AOM_QM_BITS - (1 + log_scale)))) {
        const int64_t tmp =
            abs_coeff + ROUND_POWER_OF_TWO(round_ptr[rc != 0], log_scale);
        abs_qcoeff =
            (int)((tmp * quant_ptr[rc != 0] * wt) >> (shift + AOM_QM_BITS));
        qcoeff_ptr[rc] = (tran_low_t)((abs_qcoeff ^ coeff_sign) - coeff_sign);
#if QUANT_TABLE_BITS
        const tran_low_t abs_dqcoeff =
            (tran_low_t)ROUND_POWER_OF_TWO_64((tran_high_t)abs_qcoeff * dequant,
                                              QUANT_TABLE_BITS) >>
            log_scale;
#else
        const tran_low_t abs_dqcoeff = (abs_qcoeff * dequant) >> log_scale;
#endif
        dqcoeff_ptr[rc] = (tran_low_t)((abs_dqcoeff ^ coeff_sign) - coeff_sign);
        if (abs_qcoeff) eob = i;
      } else {
        qcoeff_ptr[rc] = 0;
        dqcoeff_ptr[rc] = 0;
      }
    }
  } else {
    const int log_scaled_round_arr[2] = {
      ROUND_POWER_OF_TWO(round_ptr[0], log_scale),
      ROUND_POWER_OF_TWO(round_ptr[1], log_scale),
    };
    for (i = 0; i < count; i++) {
      const int rc = scan[i];
      const int coeff = coeff_ptr[rc];
      const int rc01 = (rc != 0);
      const int coeff_sign = (coeff >> 31);
      const int abs_coeff = (coeff ^ coeff_sign) - coeff_sign;
      const int log_scaled_round = log_scaled_round_arr[rc01];
      if (((tran_high_t)abs_coeff << (1 + log_scale + QUANT_TABLE_BITS)) >=
          (tran_high_t)dequant_ptr[rc01]) {
        const int quant = quant_ptr[rc01];
        const int dequant = dequant_ptr[rc01];
        const int64_t tmp = (int64_t)abs_coeff + log_scaled_round;
        const int abs_qcoeff = (int)((tmp * quant) >> shift);
        qcoeff_ptr[rc] = (tran_low_t)((abs_qcoeff ^ coeff_sign) - coeff_sign);
#if QUANT_TABLE_BITS
        const tran_low_t abs_dqcoeff =
            (tran_low_t)ROUND_POWER_OF_TWO_64((tran_high_t)abs_qcoeff * dequant,
                                              QUANT_TABLE_BITS) >>
            log_scale;
#else
        const tran_low_t abs_dqcoeff = (abs_qcoeff * dequant) >> log_scale;
#endif
        if (abs_qcoeff) eob = i;
        dqcoeff_ptr[rc] = (tran_low_t)((abs_dqcoeff ^ coeff_sign) - coeff_sign);
      } else {
        qcoeff_ptr[rc] = 0;
        dqcoeff_ptr[rc] = 0;
      }
    }
  }
  *eob_ptr = eob + 1;
}

#if CONFIG_EXTQUANT
void av1_quantize_fp_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs,
                       const int32_t *zbin_ptr, const int32_t *round_ptr,
                       const int32_t *quant_ptr, const int32_t *quant_shift_ptr,
                       tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr,
                       const int32_t *dequant_ptr, uint16_t *eob_ptr,
                       const int16_t *scan, const int16_t *iscan) {
#else
void av1_quantize_fp_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs,
                       const int16_t *zbin_ptr, const int16_t *round_ptr,
                       const int16_t *quant_ptr, const int16_t *quant_shift_ptr,
                       tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr,
                       const int16_t *dequant_ptr, uint16_t *eob_ptr,
                       const int16_t *scan, const int16_t *iscan) {
#endif
  quantize_fp_helper_c(coeff_ptr, n_coeffs, zbin_ptr, round_ptr, quant_ptr,
                       quant_shift_ptr, qcoeff_ptr, dqcoeff_ptr, dequant_ptr,
                       eob_ptr, scan, iscan, NULL, NULL, 0);
}

#if CONFIG_EXTQUANT
void av1_quantize_fp_32x32_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs,
                             const int32_t *zbin_ptr, const int32_t *round_ptr,
                             const int32_t *quant_ptr,
                             const int32_t *quant_shift_ptr,
                             tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr,
                             const int32_t *dequant_ptr, uint16_t *eob_ptr,
                             const int16_t *scan, const int16_t *iscan) {
#else
void av1_quantize_fp_32x32_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs,
                             const int16_t *zbin_ptr, const int16_t *round_ptr,
                             const int16_t *quant_ptr,
                             const int16_t *quant_shift_ptr,
                             tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr,
                             const int16_t *dequant_ptr, uint16_t *eob_ptr,
                             const int16_t *scan, const int16_t *iscan) {
#endif
  quantize_fp_helper_c(coeff_ptr, n_coeffs, zbin_ptr, round_ptr, quant_ptr,
                       quant_shift_ptr, qcoeff_ptr, dqcoeff_ptr, dequant_ptr,
                       eob_ptr, scan, iscan, NULL, NULL, 1);
}

#if CONFIG_EXTQUANT
void av1_quantize_fp_64x64_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs,
                             const int32_t *zbin_ptr, const int32_t *round_ptr,
                             const int32_t *quant_ptr,
                             const int32_t *quant_shift_ptr,
                             tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr,
                             const int32_t *dequant_ptr, uint16_t *eob_ptr,
                             const int16_t *scan, const int16_t *iscan) {
#else
void av1_quantize_fp_64x64_c(const tran_low_t *coeff_ptr, intptr_t n_coeffs,
                             const int16_t *zbin_ptr, const int16_t *round_ptr,
                             const int16_t *quant_ptr,
                             const int16_t *quant_shift_ptr,
                             tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr,
                             const int16_t *dequant_ptr, uint16_t *eob_ptr,
                             const int16_t *scan, const int16_t *iscan) {
#endif
  quantize_fp_helper_c(coeff_ptr, n_coeffs, zbin_ptr, round_ptr, quant_ptr,
                       quant_shift_ptr, qcoeff_ptr, dqcoeff_ptr, dequant_ptr,
                       eob_ptr, scan, iscan, NULL, NULL, 2);
}

void av1_quantize_fp_facade(const tran_low_t *coeff_ptr, intptr_t n_coeffs,
                            const MACROBLOCK_PLANE *p, tran_low_t *qcoeff_ptr,
                            tran_low_t *dqcoeff_ptr, uint16_t *eob_ptr,
                            const SCAN_ORDER *sc, const QUANT_PARAM *qparam) {
  const qm_val_t *qm_ptr = qparam->qmatrix;
  const qm_val_t *iqm_ptr = qparam->iqmatrix;
  if (qm_ptr != NULL && iqm_ptr != NULL) {
    quantize_fp_helper_c(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_fp_QTX,
                         p->quant_fp_QTX, p->quant_shift_QTX, qcoeff_ptr,
                         dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                         sc->iscan, qm_ptr, iqm_ptr, qparam->log_scale);
  } else {
    switch (qparam->log_scale) {
      case 0:
        av1_quantize_fp(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_fp_QTX,
                        p->quant_fp_QTX, p->quant_shift_QTX, qcoeff_ptr,
                        dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                        sc->iscan);
        break;
      case 1:
        av1_quantize_fp_32x32(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_fp_QTX,
                              p->quant_fp_QTX, p->quant_shift_QTX, qcoeff_ptr,
                              dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                              sc->iscan);
        break;
      case 2:
        av1_quantize_fp_64x64(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_fp_QTX,
                              p->quant_fp_QTX, p->quant_shift_QTX, qcoeff_ptr,
                              dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                              sc->iscan);
        break;
      default: assert(0);
    }
  }
}

void av1_quantize_b_facade(const tran_low_t *coeff_ptr, intptr_t n_coeffs,
                           const MACROBLOCK_PLANE *p, tran_low_t *qcoeff_ptr,
                           tran_low_t *dqcoeff_ptr, uint16_t *eob_ptr,
                           const SCAN_ORDER *sc, const QUANT_PARAM *qparam) {
  const qm_val_t *qm_ptr = qparam->qmatrix;
  const qm_val_t *iqm_ptr = qparam->iqmatrix;
  if (qparam->use_quant_b_adapt) {
    // TODO(sarahparker) These quantize_b optimizations need SIMD
    // implementations
    if (qm_ptr != NULL && iqm_ptr != NULL) {
      aom_quantize_b_adaptive_helper_c(
          coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
          p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX, eob_ptr,
          sc->scan, sc->iscan, qm_ptr, iqm_ptr, qparam->log_scale);
    } else {
      switch (qparam->log_scale) {
        case 0:
          aom_quantize_b_adaptive(coeff_ptr, n_coeffs, p->zbin_QTX,
                                  p->round_QTX, p->quant_QTX,
                                  p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr,
                                  p->dequant_QTX, eob_ptr, sc->scan, sc->iscan);
          break;
        case 1:
          aom_quantize_b_32x32_adaptive(
              coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
              p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX,
              eob_ptr, sc->scan, sc->iscan);
          break;
        case 2:
          aom_quantize_b_64x64_adaptive(
              coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
              p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX,
              eob_ptr, sc->scan, sc->iscan);
          break;
        default: assert(0);
      }
    }
  } else {
    if (qm_ptr != NULL && iqm_ptr != NULL) {
      aom_quantize_b_helper_c(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX,
                              p->quant_QTX, p->quant_shift_QTX, qcoeff_ptr,
                              dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                              sc->iscan, qm_ptr, iqm_ptr, qparam->log_scale);
    } else {
      switch (qparam->log_scale) {
        case 0:
          aom_quantize_b(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX,
                         p->quant_QTX, p->quant_shift_QTX, qcoeff_ptr,
                         dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                         sc->iscan);
          break;
        case 1:
          aom_quantize_b_32x32(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX,
                               p->quant_QTX, p->quant_shift_QTX, qcoeff_ptr,
                               dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                               sc->iscan);
          break;
        case 2:
          aom_quantize_b_64x64(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX,
                               p->quant_QTX, p->quant_shift_QTX, qcoeff_ptr,
                               dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                               sc->iscan);
          break;
        default: assert(0);
      }
    }
  }
}

#if CONFIG_EXTQUANT
static void quantize_dc(const tran_low_t *coeff_ptr, int n_coeffs,
                        int skip_block, const int32_t *round_ptr,
                        const int32_t quant, tran_low_t *qcoeff_ptr,
                        tran_low_t *dqcoeff_ptr, const int32_t dequant_ptr,
                        uint16_t *eob_ptr, const qm_val_t *qm_ptr,
                        const qm_val_t *iqm_ptr, const int log_scale) {
#else
static void quantize_dc(const tran_low_t *coeff_ptr, int n_coeffs,
                        int skip_block, const int16_t *round_ptr,
                        const int16_t quant, tran_low_t *qcoeff_ptr,
                        tran_low_t *dqcoeff_ptr, const int16_t dequant_ptr,
                        uint16_t *eob_ptr, const qm_val_t *qm_ptr,
                        const qm_val_t *iqm_ptr, const int log_scale) {
#endif
  const int rc = 0;
  const int coeff = coeff_ptr[rc];
  const int coeff_sign = (coeff >> 31);
  const int abs_coeff = (coeff ^ coeff_sign) - coeff_sign;
  int64_t tmp;
  int eob = -1;
  int32_t tmp32;
  int dequant;

  memset(qcoeff_ptr, 0, n_coeffs * sizeof(*qcoeff_ptr));
  memset(dqcoeff_ptr, 0, n_coeffs * sizeof(*dqcoeff_ptr));

  if (!skip_block) {
    const int wt = qm_ptr != NULL ? qm_ptr[rc] : (1 << AOM_QM_BITS);
    const int iwt = iqm_ptr != NULL ? iqm_ptr[rc] : (1 << AOM_QM_BITS);
    tmp = clamp(abs_coeff + ROUND_POWER_OF_TWO(round_ptr[rc != 0], log_scale),
                INT16_MIN, INT16_MAX);
    tmp32 = (int32_t)((tmp * wt * quant) >> (16 - log_scale + AOM_QM_BITS));
    qcoeff_ptr[rc] = (tmp32 ^ coeff_sign) - coeff_sign;
    dequant = (dequant_ptr * iwt + (1 << (AOM_QM_BITS - 1))) >> AOM_QM_BITS;
#if QUANT_TABLE_BITS
    const tran_low_t abs_dqcoeff =
        (tran_low_t)ROUND_POWER_OF_TWO_64((tran_high_t)tmp32 * dequant,
                                          QUANT_TABLE_BITS) >>
        log_scale;
#else
    const tran_low_t abs_dqcoeff = (tmp32 * dequant) >> log_scale;
#endif
    dqcoeff_ptr[rc] = (tran_low_t)((abs_dqcoeff ^ coeff_sign) - coeff_sign);
    if (tmp32) eob = 0;
  }
  *eob_ptr = eob + 1;
}

void av1_quantize_dc_facade(const tran_low_t *coeff_ptr, intptr_t n_coeffs,
                            const MACROBLOCK_PLANE *p, tran_low_t *qcoeff_ptr,
                            tran_low_t *dqcoeff_ptr, uint16_t *eob_ptr,
                            const SCAN_ORDER *sc, const QUANT_PARAM *qparam) {
  // obsolete skip_block
  const int skip_block = 0;
  (void)sc;
  assert(qparam->log_scale >= 0 && qparam->log_scale < (3));
  const qm_val_t *qm_ptr = qparam->qmatrix;
  const qm_val_t *iqm_ptr = qparam->iqmatrix;
  quantize_dc(coeff_ptr, (int)n_coeffs, skip_block, p->round_QTX,
              p->quant_fp_QTX[0], qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX[0],
              eob_ptr, qm_ptr, iqm_ptr, qparam->log_scale);
}

void av1_highbd_quantize_fp_facade(const tran_low_t *coeff_ptr,
                                   intptr_t n_coeffs, const MACROBLOCK_PLANE *p,
                                   tran_low_t *qcoeff_ptr,
                                   tran_low_t *dqcoeff_ptr, uint16_t *eob_ptr,
                                   const SCAN_ORDER *sc,
                                   const QUANT_PARAM *qparam) {
  const qm_val_t *qm_ptr = qparam->qmatrix;
  const qm_val_t *iqm_ptr = qparam->iqmatrix;
  if (qm_ptr != NULL && iqm_ptr != NULL) {
    highbd_quantize_fp_helper_c(
        coeff_ptr, n_coeffs, p->zbin_QTX, p->round_fp_QTX, p->quant_fp_QTX,
        p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX, eob_ptr,
        sc->scan, sc->iscan, qm_ptr, iqm_ptr, qparam->log_scale);
  } else {
    av1_highbd_quantize_fp(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_fp_QTX,
                           p->quant_fp_QTX, p->quant_shift_QTX, qcoeff_ptr,
                           dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                           sc->iscan, qparam->log_scale);
  }
}

void av1_highbd_quantize_b_facade(const tran_low_t *coeff_ptr,
                                  intptr_t n_coeffs, const MACROBLOCK_PLANE *p,
                                  tran_low_t *qcoeff_ptr,
                                  tran_low_t *dqcoeff_ptr, uint16_t *eob_ptr,
                                  const SCAN_ORDER *sc,
                                  const QUANT_PARAM *qparam) {
  const qm_val_t *qm_ptr = qparam->qmatrix;
  const qm_val_t *iqm_ptr = qparam->iqmatrix;
  if (qparam->use_quant_b_adapt) {
    if (qm_ptr != NULL && iqm_ptr != NULL) {
      aom_highbd_quantize_b_adaptive_helper_c(
          coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
          p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX, eob_ptr,
          sc->scan, sc->iscan, qm_ptr, iqm_ptr, qparam->log_scale);
    } else {
      switch (qparam->log_scale) {
        case 0:
          aom_highbd_quantize_b_adaptive(
              coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
              p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX,
              eob_ptr, sc->scan, sc->iscan);
          break;
        case 1:
          aom_highbd_quantize_b_32x32_adaptive(
              coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
              p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX,
              eob_ptr, sc->scan, sc->iscan);
          break;
        case 2:
          aom_highbd_quantize_b_64x64_adaptive(
              coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
              p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX,
              eob_ptr, sc->scan, sc->iscan);
          break;
        default: assert(0);
      }
    }
  } else {
    if (qm_ptr != NULL && iqm_ptr != NULL) {
      aom_highbd_quantize_b_helper_c(
          coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
          p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX, eob_ptr,
          sc->scan, sc->iscan, qm_ptr, iqm_ptr, qparam->log_scale);
    } else {
      switch (qparam->log_scale) {
        case 0:
          aom_highbd_quantize_b(coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX,
                                p->quant_QTX, p->quant_shift_QTX, qcoeff_ptr,
                                dqcoeff_ptr, p->dequant_QTX, eob_ptr, sc->scan,
                                sc->iscan);
          break;
        case 1:
          aom_highbd_quantize_b_32x32(
              coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
              p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX,
              eob_ptr, sc->scan, sc->iscan);
          break;
        case 2:
          aom_highbd_quantize_b_64x64(
              coeff_ptr, n_coeffs, p->zbin_QTX, p->round_QTX, p->quant_QTX,
              p->quant_shift_QTX, qcoeff_ptr, dqcoeff_ptr, p->dequant_QTX,
              eob_ptr, sc->scan, sc->iscan);
          break;
        default: assert(0);
      }
    }
  }
}

#if CONFIG_EXTQUANT
static INLINE void highbd_quantize_dc(
    const tran_low_t *coeff_ptr, int n_coeffs, int skip_block,
    const int32_t *round_ptr, const int32_t quant, tran_low_t *qcoeff_ptr,
    tran_low_t *dqcoeff_ptr, const int32_t dequant_ptr, uint16_t *eob_ptr,
    const qm_val_t *qm_ptr, const qm_val_t *iqm_ptr, const int log_scale) {
#else
static INLINE void highbd_quantize_dc(
    const tran_low_t *coeff_ptr, int n_coeffs, int skip_block,
    const int16_t *round_ptr, const int16_t quant, tran_low_t *qcoeff_ptr,
    tran_low_t *dqcoeff_ptr, const int16_t dequant_ptr, uint16_t *eob_ptr,
    const qm_val_t *qm_ptr, const qm_val_t *iqm_ptr, const int log_scale) {
#endif
  int eob = -1;

  memset(qcoeff_ptr, 0, n_coeffs * sizeof(*qcoeff_ptr));
  memset(dqcoeff_ptr, 0, n_coeffs * sizeof(*dqcoeff_ptr));

  if (!skip_block) {
    const qm_val_t wt = qm_ptr != NULL ? qm_ptr[0] : (1 << AOM_QM_BITS);
    const qm_val_t iwt = iqm_ptr != NULL ? iqm_ptr[0] : (1 << AOM_QM_BITS);
    const int coeff = coeff_ptr[0];
    const int coeff_sign = (coeff >> 31);
    const int abs_coeff = (coeff ^ coeff_sign) - coeff_sign;
    const int64_t tmp = abs_coeff + ROUND_POWER_OF_TWO(round_ptr[0], log_scale);
    const int64_t tmpw = tmp * wt;
    const int abs_qcoeff =
        (int)((tmpw * quant) >> (16 - log_scale + AOM_QM_BITS));
    qcoeff_ptr[0] = (tran_low_t)((abs_qcoeff ^ coeff_sign) - coeff_sign);
    const int dequant =
        (dequant_ptr * iwt + (1 << (AOM_QM_BITS - 1))) >> AOM_QM_BITS;

#if QUANT_TABLE_BITS
    const tran_low_t abs_dqcoeff =
        (tran_low_t)ROUND_POWER_OF_TWO_64((tran_high_t)abs_qcoeff * dequant,
                                          QUANT_TABLE_BITS) >>
        log_scale;
#else
    const tran_low_t abs_dqcoeff = (abs_qcoeff * dequant) >> log_scale;
#endif
    dqcoeff_ptr[0] = (tran_low_t)((abs_dqcoeff ^ coeff_sign) - coeff_sign);
    if (abs_qcoeff) eob = 0;
  }
  *eob_ptr = eob + 1;
}

void av1_highbd_quantize_dc_facade(const tran_low_t *coeff_ptr,
                                   intptr_t n_coeffs, const MACROBLOCK_PLANE *p,
                                   tran_low_t *qcoeff_ptr,
                                   tran_low_t *dqcoeff_ptr, uint16_t *eob_ptr,
                                   const SCAN_ORDER *sc,
                                   const QUANT_PARAM *qparam) {
  // obsolete skip_block
  const int skip_block = 0;
  const qm_val_t *qm_ptr = qparam->qmatrix;
  const qm_val_t *iqm_ptr = qparam->iqmatrix;
  (void)sc;

  highbd_quantize_dc(coeff_ptr, (int)n_coeffs, skip_block, p->round_QTX,
                     p->quant_fp_QTX[0], qcoeff_ptr, dqcoeff_ptr,
                     p->dequant_QTX[0], eob_ptr, qm_ptr, iqm_ptr,
                     qparam->log_scale);
}

#if CONFIG_EXTQUANT
void av1_highbd_quantize_fp_c(const tran_low_t *coeff_ptr, intptr_t count,
                              const int32_t *zbin_ptr, const int32_t *round_ptr,
                              const int32_t *quant_ptr,
                              const int32_t *quant_shift_ptr,
                              tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr,
                              const int32_t *dequant_ptr, uint16_t *eob_ptr,
                              const int16_t *scan, const int16_t *iscan,
                              int log_scale) {
#else
void av1_highbd_quantize_fp_c(const tran_low_t *coeff_ptr, intptr_t count,
                              const int16_t *zbin_ptr, const int16_t *round_ptr,
                              const int16_t *quant_ptr,
                              const int16_t *quant_shift_ptr,
                              tran_low_t *qcoeff_ptr, tran_low_t *dqcoeff_ptr,
                              const int16_t *dequant_ptr, uint16_t *eob_ptr,
                              const int16_t *scan, const int16_t *iscan,
                              int log_scale) {
#endif
  highbd_quantize_fp_helper_c(coeff_ptr, count, zbin_ptr, round_ptr, quant_ptr,
                              quant_shift_ptr, qcoeff_ptr, dqcoeff_ptr,
                              dequant_ptr, eob_ptr, scan, iscan, NULL, NULL,
                              log_scale);
}

#if CONFIG_EXTQUANT
static void invert_quant(int32_t *quant, int32_t *shift, int d) {
#else
static void invert_quant(int16_t *quant, int16_t *shift, int d) {
#endif
  uint32_t t;
  int l, m;
  t = d;
  for (l = 0; t > 1; l++) t >>= 1;
  m = 1 + (1u << (16 + l)) / d;
#if CONFIG_EXTQUANT
  *quant = (int32_t)(m - (1 << 16));
#else
  *quant = (int16_t)(m - (1 << 16));
#endif
  *shift = 1 << (16 - l + QUANT_TABLE_BITS);
}

static int get_qzbin_factor(int q,
#if CONFIG_DELTA_DCQUANT
                            int base_y_dc_delta_q,
#endif  // CONFIG_DELTA_DCQUANT
                            aom_bit_depth_t bit_depth) {
  const int quant = av1_dc_quant_QTX(q, 0,
#if CONFIG_DELTA_DCQUANT
                                     base_y_dc_delta_q,
#endif  // CONFIG_DELTA_DCQUANT
                                     bit_depth);
  switch (bit_depth) {
    case AOM_BITS_8:
      return q == 0 ? 64 : (quant < (148 << QUANT_TABLE_BITS) ? 84 : 80);
    case AOM_BITS_10:
      return q == 0 ? 64 : (quant < (592 << QUANT_TABLE_BITS) ? 84 : 80);
    case AOM_BITS_12:
      return q == 0 ? 64 : (quant < (2368 << QUANT_TABLE_BITS) ? 84 : 80);
    default:
      assert(0 && "bit_depth should be AOM_BITS_8, AOM_BITS_10 or AOM_BITS_12");
      return -1;
  }
}

void av1_build_quantizer(aom_bit_depth_t bit_depth, int y_dc_delta_q,
                         int u_dc_delta_q, int u_ac_delta_q, int v_dc_delta_q,
                         int v_ac_delta_q,
#if CONFIG_DELTA_DCQUANT
                         int base_y_dc_delta_q, int base_uv_dc_delta_q,
#endif  // CONFIG_DELTA_DCQUANT
                         QUANTS *const quants, Dequants *const deq) {
  int i, q, quant_QTX;

#if CONFIG_EXTQUANT_HBD
  int qindex_offset = 30 * (bit_depth - 8);
  int qindex_range = QINDEX_RANGE_8_BITS + qindex_offset;
  for (q = 0; q < qindex_range; q++) {
#else
  for (q = 0; q < QINDEX_RANGE; q++) {
#endif
    const int qzbin_factor = get_qzbin_factor(q,
#if CONFIG_DELTA_DCQUANT
                                              base_y_dc_delta_q,
#endif  // CONFIG_DELTA_DCQUANT
                                              bit_depth);
    const int qrounding_factor = q == 0 ? 64 : 48;

    for (i = 0; i < 2; ++i) {
      int qrounding_factor_fp = 64;
      // y quantizer with TX scale
      quant_QTX = i == 0 ? av1_dc_quant_QTX(q, y_dc_delta_q,
#if CONFIG_DELTA_DCQUANT
                                            base_y_dc_delta_q,
#endif  // CONFIG_DELTA_DCQUANT
                                            bit_depth)
                         : av1_ac_quant_QTX(q, 0, bit_depth);
      invert_quant(&quants->y_quant[q][i], &quants->y_quant_shift[q][i],
                   quant_QTX);
      quants->y_quant_fp[q][i] = (1 << (16 + QUANT_TABLE_BITS)) / quant_QTX;
      quants->y_round_fp[q][i] =
          (qrounding_factor_fp * quant_QTX) >> (7 + QUANT_TABLE_BITS);
      quants->y_zbin[q][i] =
          ROUND_POWER_OF_TWO(qzbin_factor * quant_QTX, (7 + QUANT_TABLE_BITS));
      quants->y_round[q][i] =
          (qrounding_factor * quant_QTX) >> (7 + QUANT_TABLE_BITS);
      deq->y_dequant_QTX[q][i] = quant_QTX;

      // u quantizer with TX scale
      quant_QTX = i == 0 ? av1_dc_quant_QTX(q, u_dc_delta_q,
#if CONFIG_DELTA_DCQUANT
                                            base_uv_dc_delta_q,
#endif  // CONFIG_DELTA_DCQUANT
                                            bit_depth)
                         : av1_ac_quant_QTX(q, u_ac_delta_q, bit_depth);
      invert_quant(&quants->u_quant[q][i], &quants->u_quant_shift[q][i],
                   quant_QTX);
      quants->u_quant_fp[q][i] = (1 << (16 + QUANT_TABLE_BITS)) / quant_QTX;
      quants->u_round_fp[q][i] =
          (qrounding_factor_fp * quant_QTX) >> (7 + QUANT_TABLE_BITS);
      quants->u_zbin[q][i] =
          ROUND_POWER_OF_TWO(qzbin_factor * quant_QTX, (7 + QUANT_TABLE_BITS));
      quants->u_round[q][i] =
          (qrounding_factor * quant_QTX) >> (7 + QUANT_TABLE_BITS);
      deq->u_dequant_QTX[q][i] = quant_QTX;

      // v quantizer with TX scale
      quant_QTX = i == 0 ? av1_dc_quant_QTX(q, v_dc_delta_q,
#if CONFIG_DELTA_DCQUANT
                                            base_uv_dc_delta_q,
#endif  // CONFIG_DELTA_DCQUANT
                                            bit_depth)
                         : av1_ac_quant_QTX(q, v_ac_delta_q, bit_depth);
      invert_quant(&quants->v_quant[q][i], &quants->v_quant_shift[q][i],
                   quant_QTX);
      quants->v_quant_fp[q][i] = (1 << (16 + QUANT_TABLE_BITS)) / quant_QTX;
      quants->v_round_fp[q][i] =
          (qrounding_factor_fp * quant_QTX) >> (7 + QUANT_TABLE_BITS);
      quants->v_zbin[q][i] =
          ROUND_POWER_OF_TWO(qzbin_factor * quant_QTX, (7 + QUANT_TABLE_BITS));
      quants->v_round[q][i] =
          (qrounding_factor * quant_QTX) >> (7 + QUANT_TABLE_BITS);
      deq->v_dequant_QTX[q][i] = quant_QTX;
    }

    for (i = 2; i < 8; i++) {  // 8: SIMD width
      quants->y_quant[q][i] = quants->y_quant[q][1];
      quants->y_quant_fp[q][i] = quants->y_quant_fp[q][1];
      quants->y_round_fp[q][i] = quants->y_round_fp[q][1];
      quants->y_quant_shift[q][i] = quants->y_quant_shift[q][1];
      quants->y_zbin[q][i] = quants->y_zbin[q][1];
      quants->y_round[q][i] = quants->y_round[q][1];
      deq->y_dequant_QTX[q][i] = deq->y_dequant_QTX[q][1];

      quants->u_quant[q][i] = quants->u_quant[q][1];
      quants->u_quant_fp[q][i] = quants->u_quant_fp[q][1];
      quants->u_round_fp[q][i] = quants->u_round_fp[q][1];
      quants->u_quant_shift[q][i] = quants->u_quant_shift[q][1];
      quants->u_zbin[q][i] = quants->u_zbin[q][1];
      quants->u_round[q][i] = quants->u_round[q][1];
      deq->u_dequant_QTX[q][i] = deq->u_dequant_QTX[q][1];
      quants->v_quant[q][i] = quants->u_quant[q][1];
      quants->v_quant_fp[q][i] = quants->v_quant_fp[q][1];
      quants->v_round_fp[q][i] = quants->v_round_fp[q][1];
      quants->v_quant_shift[q][i] = quants->v_quant_shift[q][1];
      quants->v_zbin[q][i] = quants->v_zbin[q][1];
      quants->v_round[q][i] = quants->v_round[q][1];
      deq->v_dequant_QTX[q][i] = deq->v_dequant_QTX[q][1];
    }
  }
}

void av1_init_quantizer(AV1_COMP *cpi) {
  AV1_COMMON *const cm = &cpi->common;
  QUANTS *const quants = &cpi->quants;
  Dequants *const dequants = &cpi->dequants;
  av1_build_quantizer(
      cm->seq_params.bit_depth, cm->y_dc_delta_q, cm->u_dc_delta_q,
      cm->u_ac_delta_q, cm->v_dc_delta_q, cm->v_ac_delta_q,
#if CONFIG_DELTA_DCQUANT
      cm->seq_params.base_y_dc_delta_q, cm->seq_params.base_uv_dc_delta_q,
#endif  // CONFIG_DELTA_DCQUANT
      quants, dequants);
}

void av1_init_plane_quantizers(const AV1_COMP *cpi, MACROBLOCK *x,
                               int segment_id) {
  const AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  const QUANTS *const quants = &cpi->quants;
#if CONFIG_EXTQUANT_HBD
  int current_qindex =
      AOMMAX(0, AOMMIN(cm->seq_params.bit_depth == AOM_BITS_8
                           ? QINDEX_RANGE_8_BITS - 1
                           : cm->seq_params.bit_depth == AOM_BITS_10
                                 ? QINDEX_RANGE_10_BITS - 1
                                 : QINDEX_RANGE - 1,
                       cm->delta_q_info.delta_q_present_flag
                           ? cm->base_qindex + xd->delta_qindex
                           : cm->base_qindex));
  const int qindex = av1_get_qindex(&cm->seg, segment_id, current_qindex,
                                    cm->seq_params.bit_depth);
#else
  int current_qindex = AOMMAX(
      0, AOMMIN(QINDEX_RANGE - 1, cm->delta_q_info.delta_q_present_flag
                                      ? cm->base_qindex + xd->delta_qindex
                                      : cm->base_qindex));
  const int qindex = av1_get_qindex(&cm->seg, segment_id, current_qindex);
#endif
  const int rdmult = av1_compute_rd_mult(cpi, qindex + cm->y_dc_delta_q);
  int qmlevel = (xd->lossless[segment_id] || cm->using_qmatrix == 0)
                    ? NUM_QM_LEVELS - 1
                    : cm->qm_y;

  // Y
  x->plane[0].quant_QTX = quants->y_quant[qindex];
  x->plane[0].quant_fp_QTX = quants->y_quant_fp[qindex];
  x->plane[0].round_fp_QTX = quants->y_round_fp[qindex];
  x->plane[0].quant_shift_QTX = quants->y_quant_shift[qindex];
  x->plane[0].zbin_QTX = quants->y_zbin[qindex];
  x->plane[0].round_QTX = quants->y_round[qindex];
  x->plane[0].dequant_QTX = cpi->dequants.y_dequant_QTX[qindex];
  memcpy(&xd->plane[0].seg_qmatrix[segment_id], cm->gqmatrix[qmlevel][0],
         sizeof(cm->gqmatrix[qmlevel][0]));
  memcpy(&xd->plane[0].seg_iqmatrix[segment_id], cm->giqmatrix[qmlevel][0],
         sizeof(cm->giqmatrix[qmlevel][0]));

  // U
  qmlevel = (xd->lossless[segment_id] || cm->using_qmatrix == 0)
                ? NUM_QM_LEVELS - 1
                : cm->qm_u;
  {
    x->plane[1].quant_QTX = quants->u_quant[qindex];
    x->plane[1].quant_fp_QTX = quants->u_quant_fp[qindex];
    x->plane[1].round_fp_QTX = quants->u_round_fp[qindex];
    x->plane[1].quant_shift_QTX = quants->u_quant_shift[qindex];
    x->plane[1].zbin_QTX = quants->u_zbin[qindex];
    x->plane[1].round_QTX = quants->u_round[qindex];
    x->plane[1].dequant_QTX = cpi->dequants.u_dequant_QTX[qindex];
    memcpy(&xd->plane[1].seg_qmatrix[segment_id], cm->gqmatrix[qmlevel][1],
           sizeof(cm->gqmatrix[qmlevel][1]));
    memcpy(&xd->plane[1].seg_iqmatrix[segment_id], cm->giqmatrix[qmlevel][1],
           sizeof(cm->giqmatrix[qmlevel][1]));
  }
  // V
  qmlevel = (xd->lossless[segment_id] || cm->using_qmatrix == 0)
                ? NUM_QM_LEVELS - 1
                : cm->qm_v;
  {
    x->plane[2].quant_QTX = quants->v_quant[qindex];
    x->plane[2].quant_fp_QTX = quants->v_quant_fp[qindex];
    x->plane[2].round_fp_QTX = quants->v_round_fp[qindex];
    x->plane[2].quant_shift_QTX = quants->v_quant_shift[qindex];
    x->plane[2].zbin_QTX = quants->v_zbin[qindex];
    x->plane[2].round_QTX = quants->v_round[qindex];
    x->plane[2].dequant_QTX = cpi->dequants.v_dequant_QTX[qindex];
    memcpy(&xd->plane[2].seg_qmatrix[segment_id], cm->gqmatrix[qmlevel][2],
           sizeof(cm->gqmatrix[qmlevel][2]));
    memcpy(&xd->plane[2].seg_iqmatrix[segment_id], cm->giqmatrix[qmlevel][2],
           sizeof(cm->giqmatrix[qmlevel][2]));
  }
  x->skip_block = segfeature_active(&cm->seg, segment_id, SEG_LVL_SKIP);
  x->qindex = qindex;

  set_error_per_bit(x, rdmult);

  av1_initialize_me_consts(cpi, x, qindex);
}

void av1_frame_init_quantizer(AV1_COMP *cpi) {
  MACROBLOCK *const x = &cpi->td.mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  av1_init_plane_quantizers(cpi, x, xd->mi[0]->segment_id);
}

void set_frame_dc_delta_q(const AV1_COMMON *const cm, int *y_dc_delta_q,
                          int *u_dc_delta_q, int *v_dc_delta_q) {
  (void)cm;
  *y_dc_delta_q = 0;
  *u_dc_delta_q = 0;
  *v_dc_delta_q = 0;
#if CONFIG_DELTA_DCQUANT
  if (frame_is_intra_only(cm)) {
    const int is_360p_or_larger = AOMMIN(cm->width, cm->height) >= 360;
    const int is_720p_or_larger = AOMMIN(cm->width, cm->height) >= 720;
    if (!is_360p_or_larger) {
      *y_dc_delta_q = 0;
      *u_dc_delta_q = *v_dc_delta_q = 0;
    } else if (!is_720p_or_larger) {
      *y_dc_delta_q = -2;
      *u_dc_delta_q = *v_dc_delta_q = -1;
    } else {
      *y_dc_delta_q = -4;
      *u_dc_delta_q = *v_dc_delta_q = -2;
    }
  }
#endif  // CONFIG_DELTA_DCQUANT
}

void av1_set_quantizer(AV1_COMMON *cm, int q) {
  // quantizer has to be reinitialized with av1_init_quantizer() if any
  // delta_q changes.
  cm->base_qindex = AOMMAX(cm->delta_q_info.delta_q_present_flag, q);
  cm->cur_frame->base_qindex = cm->base_qindex;
  set_frame_dc_delta_q(cm, &cm->y_dc_delta_q, &cm->u_dc_delta_q,
                       &cm->v_dc_delta_q);
  cm->u_ac_delta_q = 0;
  cm->v_ac_delta_q = 0;
  cm->qm_y = aom_get_qmlevel(cm->base_qindex, cm->min_qmlevel, cm->max_qmlevel
#if CONFIG_EXTQUANT_HBD
                             ,
                             cm->seq_params.bit_depth
#endif
  );
  cm->qm_u = aom_get_qmlevel(cm->base_qindex + cm->u_ac_delta_q,
                             cm->min_qmlevel, cm->max_qmlevel
#if CONFIG_EXTQUANT_HBD
                             ,
                             cm->seq_params.bit_depth
#endif
  );

  if (!cm->seq_params.separate_uv_delta_q)
    cm->qm_v = cm->qm_u;
  else
    cm->qm_v = aom_get_qmlevel(cm->base_qindex + cm->v_ac_delta_q,
                               cm->min_qmlevel, cm->max_qmlevel
#if CONFIG_EXTQUANT_HBD
                               ,
                               cm->seq_params.bit_depth
#endif
    );
}

// Table that converts 0-63 Q-range values passed in outside to the Qindex
// range used internally.
// clang-format off
static const int quantizer_to_qindex[] = {
  0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  44,  48,
  52,  56,  60,  64,  68,  72,  76,  80,  84,  88,  92,  96,  100,
  104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 148, 152,
  156, 160, 164, 168, 172, 176, 180, 184, 188, 192, 196, 200, 204,
  208, 212, 216, 220, 224, 228, 232, 236, 240, 244, 249, 255,
};
// clang-format on

#if CONFIG_EXTQUANT_HBD
static const int qindex_10b_offset[] = {
  0,  7,  18, 34, 42, 50, 54, 56, 60, 60, 60, 60, 60, 60, 60, 60,
  60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60,
  60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60,
  60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60,
};
static const int qindex_12b_offset[] = {
  0,   7,   75,  94,  102, 110, 114, 116, 120, 120, 120, 120, 120,
  120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120,
  120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120,
  120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120,
  120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120, 120,
};
#endif

int av1_quantizer_to_qindex(int quantizer
#if CONFIG_EXTQUANT_HBD
                            ,
                            aom_bit_depth_t bit_depth
#endif
) {
#if CONFIG_EXTQUANT_HBD
  switch (bit_depth) {
    case AOM_BITS_8: return quantizer_to_qindex[quantizer];
    case AOM_BITS_10:
      return (quantizer_to_qindex[quantizer] + qindex_10b_offset[quantizer]);
    case AOM_BITS_12:
      return (quantizer_to_qindex[quantizer] + qindex_12b_offset[quantizer]);
    default:
      assert(0 && "bit_depth should be AOM_BITS_8, AOM_BITS_10 or AOM_BITS_12");
      return -1;
  }
#else
  return quantizer_to_qindex[quantizer];
#endif
}

int av1_qindex_to_quantizer(int qindex
#if CONFIG_EXTQUANT_HBD
                            ,
                            aom_bit_depth_t bit_depth
#endif
) {
  int quantizer;
  for (quantizer = 0; quantizer < 64; ++quantizer)
#if CONFIG_EXTQUANT_HBD
    if (av1_quantizer_to_qindex(quantizer, bit_depth) >= qindex)
      return quantizer;
#else
    if (quantizer_to_qindex[quantizer] >= qindex) return quantizer;
#endif

  return 63;
}
