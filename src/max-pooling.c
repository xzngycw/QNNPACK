/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <qnnpack.h>
#include <qnnpack/operator.h>
#include <qnnpack/log.h>
#include <qnnpack/common.h>
#include <qnnpack/math.h>
#include <qnnpack/params.h>


static inline size_t compute_output_dimension(
    size_t padded_input_dimension,
    size_t kernel_dimension,
    size_t dilation_dimension,
    size_t stride_dimension)
{
  const size_t effective_kernel_dimension = (kernel_dimension - 1) * dilation_dimension + 1;
  return (padded_input_dimension - effective_kernel_dimension) / stride_dimension + 1;
}

enum qnnp_status qnnp_create_max_pooling2d_nhwc_u8(
    uint32_t input_padding_top,
    uint32_t input_padding_right,
    uint32_t input_padding_bottom,
    uint32_t input_padding_left,
    uint32_t pooling_height,
    uint32_t pooling_width,
    uint32_t stride_height,
    uint32_t stride_width,
    uint32_t dilation_height,
    uint32_t dilation_width,
    size_t channels,
    uint8_t output_min,
    uint8_t output_max,
    qnnp_operator_t* max_pooling_out)
{
  qnnp_operator_t max_pooling = NULL;
  enum qnnp_status status = qnnp_status_uninitialized;

  if (!qnnp_params.initialized) {
    qnnp_log_error("qnnp_create_max_pooling2d_nhwc_q8 failed because QNNPACK is not properly initialized");
    goto error;
  }

  status = qnnp_status_invalid_parameter;

  const uint32_t pooling_size = pooling_height * pooling_width;
  if (pooling_size == 0) {
    qnnp_log_error(
      "failed to create max pooling with %" PRIu32 "x%" PRIu32 " pooling size: "
      "pooling size dimensions must be non-zero",
      pooling_width, pooling_height);
    goto error;
  }

  if (pooling_size == 1) {
    qnnp_log_error(
      "failed to create max pooling with 1 pooling element: "
      "1x1 pooling is meaningless");
    goto error;
  }

  if (stride_height == 0 || stride_width == 0) {
    qnnp_log_error(
      "failed to create max pooling with %" PRIu32 "x%" PRIu32 " stride: "
      "stride dimensions must be non-zero",
      stride_width, stride_height);
    goto error;
  }

  if (dilation_height == 0 || dilation_width == 0) {
    qnnp_log_error(
      "failed to create max pooling with %" PRIu32 "x%" PRIu32 " dilation: "
      "dilation dimensions must be non-zero",
      dilation_width, dilation_height);
    goto error;
  }

  if (channels == 0) {
    qnnp_log_error(
      "failed to create max pooling with %zu channels: "
      "number of channels must be non-zero",
      channels);
    goto error;
  }

  status = qnnp_status_out_of_memory;

  max_pooling = calloc(1, sizeof(struct qnnp_operator));
  if (max_pooling == NULL) {
    qnnp_log_error("failed to allocate %zu bytes for qnnp_operator structure", sizeof(struct qnnp_operator));
    goto error;
  }

  max_pooling->input_padding_top = input_padding_top;
  max_pooling->input_padding_right = input_padding_right;
  max_pooling->input_padding_bottom = input_padding_bottom;
  max_pooling->input_padding_left = input_padding_left;

  max_pooling->kernel_height = pooling_height;
  max_pooling->kernel_width = pooling_width;
  max_pooling->stride_height = stride_height;
  max_pooling->stride_width = stride_width;
  max_pooling->dilation_height = dilation_height;
  max_pooling->dilation_width = dilation_width;
  max_pooling->channels = channels;

  max_pooling->maxpool_quantization_params =
    qnnp_compute_maxpool_quantization_params(output_min, output_max);

  max_pooling->ukernel_type = qnnp_ukernel_type_max_pooling;
  max_pooling->format = qnnp_format_quint8;

  *max_pooling_out = max_pooling;
  return qnnp_status_success;

error:
  qnnp_delete_operator(max_pooling);
  return status;
}

enum qnnp_status qnnp_setup_max_pooling2d_nhwc_u8(
    qnnp_operator_t max_pooling,
    size_t batch_size,
    size_t input_height,
    size_t input_width,
    const uint8_t* input,
    size_t input_pixel_stride,
    uint8_t* output,
    size_t output_pixel_stride,
    pthreadpool_t threadpool)
{
  if (!qnnp_params.initialized) {
    qnnp_log_error("qnnp_setup_max_pooling2d_nhwc_q8 failed because QNNPACK is not properly initialized");
    return qnnp_status_uninitialized;
  }

  if (batch_size == 0) {
    qnnp_log_error("failed to setup max pooling with batch size %zu: batch size must be non-zero", batch_size);
    return qnnp_status_invalid_parameter;
  }

  if (input_width == 0 || input_height == 0) {
    qnnp_log_error(
      "failed to setup max pooling with %zux%zu input: input dimensions must be non-zero",
      input_width, input_height);
    return qnnp_status_invalid_parameter;
  }

  max_pooling->batch_size = batch_size;
  max_pooling->input_height = input_height;
  max_pooling->input_width = input_width;
  max_pooling->input = input;
  max_pooling->input_pixel_stride = input_pixel_stride;

  max_pooling->output_height = compute_output_dimension(
      max_pooling->input_padding_top + input_height + max_pooling->input_padding_bottom,
      max_pooling->kernel_height,
      max_pooling->dilation_height,
      max_pooling->stride_height);
  max_pooling->output_width = compute_output_dimension(
      max_pooling->input_padding_left + input_width + max_pooling->input_padding_right,
      max_pooling->kernel_width,
      max_pooling->dilation_width,
      max_pooling->stride_width);
  max_pooling->output = output;
  max_pooling->output_pixel_stride = output_pixel_stride;

  size_t valid_batch_size = 0;
  if (input == max_pooling->last_input &&
      input_height == max_pooling->last_input_height &&
      input_width == max_pooling->last_input_width)
  {
    valid_batch_size = max_pooling->valid_batch_size;
    if (batch_size <= valid_batch_size) {
      return qnnp_status_success;
    }
  }

  const size_t pooling_height = max_pooling->kernel_height;
  const size_t pooling_width = max_pooling->kernel_width;
  const size_t pooling_size = pooling_height * pooling_width;
  const size_t output_height = max_pooling->output_height;
  const size_t output_width = max_pooling->output_width;
  /* Micro-kernel may read up to (mr - 1) elements after the end of indirection buffer */
  const uint32_t mr = qnnp_params.u8maxpool.mr;

  const size_t width_step =
    max_pooling->dilation_width > 1 ? pooling_width : min(max_pooling->stride_width, pooling_width);
  const size_t indirection_buffer_size = sizeof(void*) * ((mr - 1) + batch_size * output_height *
    (pooling_size + (output_width * width_step - 1) * pooling_height));

  const void** indirection_buffer = (const void**) realloc(max_pooling->indirection_buffer, indirection_buffer_size);
  if (indirection_buffer == NULL) {
    qnnp_log_error("failed to allocate %zu bytes for indirection buffer", indirection_buffer_size);
    return qnnp_status_out_of_memory;
  }
  max_pooling->indirection_buffer = indirection_buffer;

  for (size_t image = 0; image < batch_size; image++) {
    for (size_t output_y = 0; output_y < output_height; output_y++) {
      for (size_t pooling_y = 0; pooling_y < pooling_height; pooling_y++) {
        const size_t input_y = doz(output_y * max_pooling->stride_height + pooling_y * max_pooling->dilation_height, max_pooling->input_padding_top);
        const size_t clamped_input_y = min(input_y, input_height - 1);
        for (size_t output_x = 0; output_x < output_width; output_x++) {
          for (size_t pooling_x = 0; pooling_x < pooling_width; pooling_x++) {
            const size_t input_x = doz(output_x * max_pooling->stride_width + pooling_x * max_pooling->dilation_width, max_pooling->input_padding_left);
            const size_t clamped_input_x = min(input_x, input_width - 1);
            const size_t index =
              (image * output_height + output_y) * (pooling_size + (output_width * width_step - 1) * pooling_height) +
              output_x * width_step * pooling_height + pooling_x * pooling_height + pooling_y;
            indirection_buffer[index] = input + ((image * input_height + clamped_input_y) * input_width + clamped_input_x) * input_pixel_stride;
          }
        }
      }
    }
  }
  max_pooling->last_input = input;
  max_pooling->last_input_height = input_height;
  max_pooling->last_input_width = input_width;
  max_pooling->valid_batch_size = max(valid_batch_size, batch_size);

  return qnnp_status_success;
}