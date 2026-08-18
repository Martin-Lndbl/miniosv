/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __VACCEL_OPS_H__
#define __VACCEL_OPS_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vaccel_op_type {
	VACCEL_NO_OP = 0,
	VACCEL_BLAS_SGEMM,          /* 1 */
	VACCEL_IMG_CLASS,           /* 2 */
	VACCEL_IMG_DETEC,           /* 3 */
	VACCEL_IMG_SEGME,           /* 4 */
	VACCEL_IMG_POSE,            /* 5 */
	VACCEL_IMG_DEPTH,           /* 6 */
	VACCEL_EXEC,                /* 7 */
	VACCEL_TF_MODEL_NEW,        /* 8 */
	VACCEL_TF_MODEL_DESTROY,    /* 9 */
	VACCEL_TF_MODEL_REGISTER,   /* 10 */
	VACCEL_TF_MODEL_UNREGISTER, /* 11 */
	VACCEL_TF_SESSION_LOAD,     /* 12 */
	VACCEL_TF_SESSION_RUN,      /* 13 */
	VACCEL_TF_SESSION_DELETE,   /* 14 */
	VACCEL_MINMAX,              /* 15 */
	VACCEL_F_ARRAYCOPY,	    /* 16 */
	VACCEL_F_MMULT,		    /* 17 */
	VACCEL_F_PARALLEL,	    /* 18 */
	VACCEL_F_VECTORADD,	    /* 19 */
	VACCEL_EXEC_WITH_RESOURCE,
	VACCEL_TORCH_JITLOAD_FORWARD,
	VACCEL_TORCH_SGEMM,
	VACCEL_OPENCV,
	VACCEL_TFLITE_SESSION_LOAD,
	VACCEL_TFLITE_SESSION_RUN,
	VACCEL_TFLITE_SESSION_DELETE,
	VACCEL_MATMUL_CREATE,
	VACCEL_CREATE_MEM,
	VACCEL_DESTROY_MEM,
	VACCEL_MATMUL_DESTROY,
	VACCEL_MATMUL_SET_IO,
	VACCEL_MATMUL_SET_CORE_MASK,
	VACCEL_MATMUL_RUN,
	VACCEL_MATMUL_SET_MATRIX,
	VACCEL_MATMUL_GET_MATRIX,
	VACCEL_MATMUL_GET_PROPS,
	VACCEL_FUNCTIONS_NR
};

static const char *vaccel_op_name[] = {
	"noop",
	"sgemm",
	"image classification",
	"image detection",
	"image segmentation",
	"image pose estimation",
	"image depth estimation",
	"exec",
	"TensorFlow model create",
	"TensorFlow model destroy",
	"TensorFlow model register",
	"TensorFlow model unregister",
	"TensorFlow session load",
	"TensorFlow session run",
	"TensorFlow session delete",
	"MinMax",
	"Array copy",
	"Matrix multiplication",
	"Parallel acceleration",
	"Vector Add",
    "Exec with resource",
    "Torch jitload forward",
    "Torch SGEMM",
    "OpenCV",
    "TFlite session load",
    "TFlite session run",
    "TFlite session delete",
    "Matmul create",
    "Create mem",
    "Destroy mem",
    "Matmul destroy",
    "Matmul set IO",
    "Matmul set core mask",
    "Matmul run",
    "Matmul set matrix",
    "Matmul get matrix",
    "Matmul get props",
};

static inline const char *vaccel_op_type_str(enum vaccel_op_type op_type)
{
	return vaccel_op_name[op_type];
}

#ifdef __cplusplus
}
#endif

#endif /* __VACCEL_OPS_H__ */
