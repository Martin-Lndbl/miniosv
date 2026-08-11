// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vaccel_session;
/*
Definition for tensor
*/
#define VACCEL_MAX_DIMS 16 /* maximum dimension of tensor. */
#define VACCEL_MAX_NUM_CHANNEL 15 /* maximum channel number of input tensor. */
#define VACCEL_MAX_NAME_LEN 256 /* maximum name lenth of tensor. */
#define VACCEL_MAX_DYNAMIC_SHAPE_NUM \
	512 /* maximum number of dynamic shape for each input. */

#ifdef __arm__
typedef uint32_t vaccel_matmul_ctx;
#else
typedef uint64_t vaccel_matmul_ctx;
#endif

typedef enum _vaccel_tensor_type {
	VACCEL_TENSOR_FLOAT32 = 0, /* data type is float32. */
	VACCEL_TENSOR_FLOAT16, /* data type is float16. */
	VACCEL_TENSOR_INT8, /* data type is int8. */
	VACCEL_TENSOR_UINT8, /* data type is uint8. */
	VACCEL_TENSOR_INT16, /* data type is int16. */
	VACCEL_TENSOR_UINT16, /* data type is uint16. */
	VACCEL_TENSOR_INT32, /* data type is int32. */
	VACCEL_TENSOR_UINT32, /* data type is uint32. */
	VACCEL_TENSOR_INT64, /* data type is int64. */
	VACCEL_TENSOR_BOOL,
	VACCEL_TENSOR_INT4,
	VACCEL_TENSOR_BFLOAT16,

	VACCEL_TENSOR_TYPE_MAX
} vaccel_tensor_type;

typedef enum _vaccel_matmul_type {
	VACCEL_FLOAT16_MM_FLOAT16_TO_FLOAT32 = 1,
	VACCEL_INT8_MM_INT8_TO_INT32 = 2,
	VACCEL_INT8_MM_INT8_TO_INT8 = 3,
	VACCEL_FLOAT16_MM_FLOAT16_TO_FLOAT16 = 4,
	VACCEL_FLOAT16_MM_INT8_TO_FLOAT32 = 5,
	VACCEL_FLOAT16_MM_INT8_TO_FLOAT16 = 6,
	VACCEL_FLOAT16_MM_INT4_TO_FLOAT32 = 7,
	VACCEL_FLOAT16_MM_INT4_TO_FLOAT16 = 8,
	VACCEL_INT8_MM_INT8_TO_FLOAT32 = 9,
	VACCEL_INT4_MM_INT4_TO_INT16 = 10,
	VACCEL_INT8_MM_INT4_TO_INT32 = 11,
	VACCEL_FLOAT16_MM_INT4_TO_BFLOAT16 = 12,
} vaccel_matmul_type;

typedef struct _vaccel_matmul_tensor_attr {
	char name[VACCEL_MAX_NAME_LEN];

	// indicate A(M, K) or B(K, N) or C(M, N)
	uint32_t n_dims;
	uint32_t dims[VACCEL_MAX_DIMS];

	// matmul tensor size
	uint32_t size;

	// matmul tensor data type
	// int8 : A, B
	// int32: C
	vaccel_tensor_type type;

} vaccel_matmul_tensor_attr;

typedef struct _vaccel_matmul_io_attr {
	// indicate A(M, K) or B(K, N) or C(M, N)
	vaccel_matmul_tensor_attr A;
	vaccel_matmul_tensor_attr B;
	vaccel_matmul_tensor_attr C;
} vaccel_matmul_io_attr;

typedef struct vaccel_matmul_info_t {
	int32_t M;
	int32_t K; // limit: RK3566/3568: int8 type must be aligned with 32byte, float16 type must be aligned with 16byte;
	// RK3562:      int8 type must be aligned with 32byte, float16 type must be aligned with 32byte;
	// RK3588/3576: int8 type must be aligned with 32byte, float16 type must be aligned with 32byte,
	//              int4 type must be aligned with 32byte;
	int32_t N; // limit: RK3566/3568: int8 type must be aligned with 16byte, float16 type must be aligned with 8byte;
	// RK3562:      int8 type must be aligned with 16byte, float16 type must be aligned with 8byte;
	// RK3588/3576: int8 type must be aligned with 32byte, float16 type must be aligned with 16byte,
	//              int4 type must be aligned with 64byte;
	// matmul data type
	// int4: int4(A) x int4(B) -> int16(C)
	// int8: int8(A) x int8(B) -> int32(C)
	// float16: float16(A) x float16(B) -> float32(C)
	vaccel_matmul_type type;

	// matmul native layout for B
	// 0: normal layout
	// 1: native layout
	int16_t B_layout;

	// matmul quant type for B
	// A and C only support per layer
	// 0: per layer
	// 1: per channel
	// 2: per group
	int16_t B_quant_type;

	// matmul native layout for A and C
	// 0: normal layout
	// 1: native layout
	int16_t AC_layout;

	// matmul quant type for A and C, only support 0
	int16_t AC_quant_type;

	// iommu domain id, each domain has 4GB of space
	int32_t iommu_domain_id;

	// B_quant_type set 2, group size is enable
	int16_t group_size;

	// reserved field
	int8_t reserved[34];
} vaccel_matmul_info;

typedef struct vaccel_tensor_mem_handle vaccel_tensor_mem_handle;


typedef struct vaccel_tensor_mem {
	void* virt_addr;
	vaccel_tensor_mem_handle* handle;
} vaccel_tensor_mem;

typedef enum _vaccel_core_mask {
	VACCEL_NPU_CORE_AUTO = 0,                                       /* default, run on NPU core randomly. */
	VACCEL_NPU_CORE_0 = 1,                                          /* run on NPU core 0. */
	VACCEL_NPU_CORE_1 = 2,                                          /* run on NPU core 1. */
	VACCEL_NPU_CORE_2 = 4,                                          /* run on NPU core 2. */
	VACCEL_NPU_CORE_0_1 = VACCEL_NPU_CORE_0 | VACCEL_NPU_CORE_1,        /* run on NPU core 0 and core 1. */
	VACCEL_NPU_CORE_0_1_2 = VACCEL_NPU_CORE_0_1 | VACCEL_NPU_CORE_2,    /* run on NPU core 0 and core 1 and core 2. */
	VACCEL_NPU_CORE_ALL = 0xffff,                                   /* auto choice, run on NPU cores depending on platform */

	VACCEL_NPU_CORE_UNDEFINED,
} vaccel_core_mask;

int vaccel_matmul_create(struct vaccel_session *sess, vaccel_matmul_ctx *ctx,
			 vaccel_matmul_info *info,
			 vaccel_matmul_io_attr *io_attr);

int vaccel_create_mem(struct vaccel_session *sess, vaccel_matmul_ctx ctx,
		      uint32_t size, vaccel_tensor_mem *result);

int vaccel_destroy_mem(struct vaccel_session *sess, vaccel_matmul_ctx ctx,
		       vaccel_tensor_mem *mem);

int vaccel_matmul_destroy(struct vaccel_session *sess, vaccel_matmul_ctx ctx);

int vaccel_matmul_set_io_mem(struct vaccel_session *sess, vaccel_matmul_ctx ctx,
			     vaccel_tensor_mem_handle *mem,
			     vaccel_matmul_tensor_attr *attr);

int vaccel_matmul_set_core_mask(struct vaccel_session *sess,
				vaccel_matmul_ctx ctx,
				vaccel_core_mask core_mask);

int vaccel_matmul_run(struct vaccel_session *sess, vaccel_matmul_ctx ctx);

int vaccel_matmul_set_matrix(struct vaccel_session *sess,
			     vaccel_tensor_mem_handle *dst, void *src,
			     size_t nbytes);

int vaccel_matmul_get_matrix(struct vaccel_session *sess, void *dst,
			     vaccel_tensor_mem_handle *src, size_t nbytes);

int vaccel_matmul_get_props(struct vaccel_session *sess, char *props,
			    size_t nbytes);

#ifdef __cplusplus
}
#endif
