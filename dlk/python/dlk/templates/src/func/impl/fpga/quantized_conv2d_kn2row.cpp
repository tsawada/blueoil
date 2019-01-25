/* Copyright 2018 The Blueoil Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <cassert>
#include <cstdio>

#include "de10_nano.h"
#include "func/impl/quantized_conv2d_kn2row.h"
#include "global.h"
#include "pack_input_to_qwords.h"
#include "time_measurement.h"

namespace {

const unsigned int in_nbits = 2;
const unsigned int byte_nbits = 8;

void kernel_transform_NHWC_to_HWNoCNi(const T_UINT src[], T_UINT dst[],
                                      const unsigned kn, const unsigned kh,
                                      const unsigned kw, const unsigned kc,
                                      const unsigned kn_in) {
  unsigned idx_src = 0;
  const unsigned kn_out = kn / kn_in;

  for (unsigned no = 0; no < kn_out; no++)
    for (unsigned ni = 0; ni < kn_in; ni++)
      for (unsigned h = 0; h < kh; h++)
        for (unsigned w = 0; w < kw; w++)
          for (unsigned c = 0; c < kc; c++) {
            unsigned idx_dst = h * (kw * kn_out * kc * kn_in);
            idx_dst += w * (kn_out * kc * kn_in);
            idx_dst += no * (kc * kn_in);
            idx_dst += c * (kn_in);
            idx_dst += ni;
            dst[idx_dst] = src[idx_src++];
          }
}

void kernel_transform_NHWC_to_NoHWCNi(const T_UINT src[], T_UINT dst[],
                                      const unsigned kn, const unsigned kh,
                                      const unsigned kw, const unsigned kc,
                                      const unsigned kn_in) {
  unsigned idx_src = 0;
  const unsigned kn_out = kn / kn_in;

  for (unsigned no = 0; no < kn_out; no++)
    for (unsigned ni = 0; ni < kn_in; ni++)
      for (unsigned h = 0; h < kh; h++)
        for (unsigned w = 0; w < kw; w++)
          for (unsigned c = 0; c < kc; c++) {
            unsigned idx_dst = no * (kh * kw * kc * kn_in);
            idx_dst += h * (kw * kc * kn_in);
            idx_dst += w * (kc * kn_in);
            idx_dst += c * (kn_in);
            idx_dst += ni;
            dst[idx_dst] = src[idx_src++];
          }
}

} // namespace

namespace dlk {

namespace impl {

void QuantizedConv2DKn2Row(QUANTIZED_NOT_PACKED input[], const T_UINT kernel[],
                           const binary_convolution_parameters &p) {
  using namespace dlk;

  convolution_parameters cp = p.normal_conv_params;
  const T_UINT out_c = cp.output_channels;

  const T_UINT num_qinput_per_qword = (NBIT_QDYPE / MAX_NBIT_QINPUT);
  const T_UINT num_qkernel_per_qword = (NBIT_QDYPE / MAX_NBIT_KERNEL);

  const T_UINT k_h = cp.kernel_height;
  const T_UINT k_w = cp.kernel_width;
  const T_UINT k_c = cp.kernel_depth;
  const T_UINT k_c_by_word =
      (k_c + (num_qkernel_per_qword - 1)) / num_qkernel_per_qword;
  const T_UINT k_n = out_c;

  const T_UINT in_h = cp.input_height;
  const T_UINT in_w = cp.input_width;
  const T_UINT in_c = k_c;
  const T_UINT in_c_by_word = k_c_by_word;
  const T_UINT in_size = in_h * in_w * in_c_by_word * MAX_NBIT_QINPUT;

  const T_UINT out_h = cp.output_height;
  const T_UINT out_w = cp.output_width;
  const T_UINT out_size = out_h * out_w * out_c;

  const bool out_c_less_than_num_pe = (out_c < NUM_PE);

#ifdef DLK_DEBUG
  const bool in_c_less_than_word_size = (in_c < WORD_SIZE);
  const bool in_c_excess_the_max = (in_c > MAX_IN_C);
  const bool ts_activated = (p.thresholds != nullptr);

  if (in_c_less_than_word_size) {
    std::printf("[WORNING] in_c(%u) less than word_size(%u)\n", in_c,
                WORD_SIZE);
  } else if (out_c_less_than_num_pe) {
    std::printf("[WORNING] out_c(%u) less than num_pe(%u)\n", out_c, NUM_PE);
  } else if (in_c_excess_the_max) {
    std::printf("[Fatal Error] in_c(%u) excess the max(%u)\n", in_c, MAX_IN_C);
  } else if (ts_activated) {
    std::printf("[Fatal Error] Now threshold skipping is not supported\n");
  }

  assert(!in_c_less_than_word_size);
  assert(!in_c_excess_the_max);
  assert(!ts_activated);
#endif

  Measurement::Start("Packing input for kn2row");
  const T_UINT in_size_orig = in_h * in_w * in_c;
  pack_input_to_qwords(input, p.device_input_buf, in_size_orig, 2);
  Measurement::Stop();

  if (out_c_less_than_num_pe) {
    const T_UINT k_n_aligend_with_num_pe =
        ((k_n + (NUM_PE - 1)) / NUM_PE) * NUM_PE;
    const T_UINT out_c_aligend_with_num_pe = k_n_aligend_with_num_pe;
    const T_UINT k_size = k_n_aligend_with_num_pe * k_h * k_w * k_c_by_word;

    T_UINT kernel_nohwcni[k_size];
    T_UINT kernel_filled_extra[k_size];

    for (size_t k = 0; k < k_n * k_h * k_w * k_c_by_word; k++) {
      kernel_filled_extra[k] = kernel[k];
    }

    Measurement::Start("Kernel transpose NHWC to HWNoCNi");
    kernel_transform_NHWC_to_NoHWCNi(kernel_filled_extra, kernel_nohwcni,
                                     k_n_aligend_with_num_pe, k_h, k_w,
                                     k_c_by_word, NUM_PE);
    Measurement::Stop();

    T_UINT input_byte_size =
        (cp.input_height * cp.input_width * cp.kernel_depth * in_nbits) /
        byte_nbits;

    T_UINT output_byte_size_full =
        out_h * out_w * out_c * sizeof(BIN_CONV_OUTPUT);
    T_UINT output_byte_size_partial =
        out_h * out_w * out_c_aligend_with_num_pe * sizeof(BIN_CONV_OUTPUT);

    p.dma_input_buffer->sync_size(input_byte_size);
    p.dma_output_buffer->sync_size(output_byte_size_full);

    p.dma_input_buffer->sync_for_device();
    p.dma_output_buffer->sync_for_device();

    Measurement::Start("QConv2D with kn2row");
    de10_nano::qconv_kn2row_tiling(
        p.device_input_phys_addr, p.device_output_phys_addr, kernel_nohwcni,
        p.thresholds, in_w, in_h, in_c_by_word, MAX_NBIT_QINPUT, out_w, out_h,
        out_c_aligend_with_num_pe, k_w, k_h, cp.padding,
        cp.stride_along_height);
    Measurement::Stop();

    p.dma_output_buffer->sync_size(output_byte_size_partial);
    p.dma_output_buffer->sync_for_cpu();

    unsigned idx_out_src = 0;
    unsigned idx_out_dst = 0;

    for (unsigned h = 0; h < out_h; h++)
      for (unsigned w = 0; w < out_w; w++)
        for (unsigned c = 0; c < out_c_aligend_with_num_pe; c++) {
          if (c < out_c) {
            p.device_output_buf[idx_out_dst++] =
                p.device_output_buf[idx_out_src];
          }
          idx_out_src++;
        }
  } else {

    const T_UINT k_size = k_n * k_h * k_w * k_c_by_word;
    T_UINT kernel_nohwcni[k_size];

    Measurement::Start("Kernel transpose NHWC to HWNoCNi");
    kernel_transform_NHWC_to_NoHWCNi(kernel, kernel_nohwcni, k_n, k_h, k_w,
                                     k_c_by_word, NUM_PE);
    Measurement::Stop();

    T_UINT input_byte_size =
        (cp.input_height * cp.input_width * cp.kernel_depth * in_nbits) /
        byte_nbits;

    T_UINT output_byte_size = out_h * out_w * out_c * sizeof(BIN_CONV_OUTPUT);

    p.dma_input_buffer->sync_size(input_byte_size);
    p.dma_output_buffer->sync_size(output_byte_size);

    p.dma_input_buffer->sync_for_device();
    p.dma_output_buffer->sync_for_device();

    Measurement::Start("QConv2D with kn2row");
    de10_nano::qconv_kn2row_tiling(
        p.device_input_phys_addr, p.device_output_phys_addr, kernel_nohwcni,
        p.thresholds, in_w, in_h, in_c_by_word, MAX_NBIT_QINPUT, out_w, out_h,
        out_c, k_w, k_h, cp.padding, cp.stride_along_height);
    Measurement::Stop();

    p.dma_output_buffer->sync_size(output_byte_size);
    p.dma_input_buffer->sync_size(input_byte_size);

    p.dma_input_buffer->sync_for_cpu();
    p.dma_output_buffer->sync_for_cpu();
  }
}

} // namespace impl

} // namespace dlk
