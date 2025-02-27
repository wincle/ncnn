// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2017 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "deconvolution_arm.h"
#include "layer_type.h"

#if __ARM_NEON
#include <arm_neon.h>
#endif // __ARM_NEON

namespace ncnn {

#include "deconvolution_4x4.h"
#include "deconvolution_3x3.h"

DEFINE_LAYER_CREATOR(Deconvolution_arm)

Deconvolution_arm::Deconvolution_arm()
{
    activation = 0;
}

int Deconvolution_arm::create_pipeline(const Option& opt)
{
    if (activation_type == 1)
    {
        activation = ncnn::create_layer(ncnn::LayerType::ReLU);

        ncnn::ParamDict pd;
        activation->load_param(pd);
    }
    else if (activation_type == 2)
    {
        activation = ncnn::create_layer(ncnn::LayerType::ReLU);

        ncnn::ParamDict pd;
        pd.set(0, activation_params[0]);// slope
        activation->load_param(pd);
    }
    else if (activation_type == 3)
    {
        activation = ncnn::create_layer(ncnn::LayerType::Clip);

        ncnn::ParamDict pd;
        pd.set(0, activation_params[0]);// min
        pd.set(1, activation_params[1]);// max
        activation->load_param(pd);
    }
    else if (activation_type == 4)
    {
        activation = ncnn::create_layer(ncnn::LayerType::Sigmoid);

        ncnn::ParamDict pd;
        activation->load_param(pd);
    }

    if (activation)
    {
        Option opt_cpu = opt;
        opt_cpu.use_vulkan_compute = false;
        activation->create_pipeline(opt_cpu);
    }

    return 0;
}

int Deconvolution_arm::destroy_pipeline(const Option& opt)
{
    if (activation)
    {
        Option opt_cpu = opt;
        opt_cpu.use_vulkan_compute = false;
        activation->destroy_pipeline(opt_cpu);
        delete activation;
        activation = 0;
    }

    return 0;
}

int Deconvolution_arm::forward(const Mat& bottom_blob, Mat& top_blob, const Option& opt) const
{
    // deconvolv with NxN kernel
    // value = value + bias

    if (kernel_w != kernel_h || stride_w != stride_h)
    {
        return Deconvolution::forward(bottom_blob, top_blob, opt);
    }

    const int kernel_size = kernel_w;
    const int stride = stride_w;

    if ((kernel_size != 3 && kernel_size != 4) || stride > 2 || dilation_w != 1 || dilation_h != 1)
    {
        return Deconvolution::forward(bottom_blob, top_blob, opt);
    }

    typedef void (*deconv_func)(const Mat&, Mat&, const Mat&, const Mat&, const Option&);

    // kernel_size x stride
    deconv_func deconv_func_table[2][2] =
    {
        {
            deconv3x3s1_neon,
            deconv3x3s2_neon
        },  // kernel_size = 3
        {
            deconv4x4s1_neon,
            deconv4x4s2_neon
        }   // kernel_size = 4
    };

    deconv_func deconv = deconv_func_table[kernel_size-3][stride-1];
    if (!deconv)
    {
        return Deconvolution::forward(bottom_blob, top_blob, opt);
    }

    int w = bottom_blob.w;
    int h = bottom_blob.h;
    size_t elemsize = bottom_blob.elemsize;

    int outw = (w - 1) * stride + kernel_size + output_pad_w;
    int outh = (h - 1) * stride + kernel_size + output_pad_h;

    Mat top_blob_bordered;
    if (pad_w > 0 || pad_h > 0 || output_pad_w > 0 || output_pad_h > 0)
    {
        top_blob_bordered.create(outw, outh, num_output, elemsize, opt.workspace_allocator);
        if (top_blob_bordered.empty())
            return -100;
    }
    else
    {
        top_blob_bordered = top_blob;
        top_blob_bordered.create(outw, outh, num_output, elemsize, opt.blob_allocator);
        if (top_blob_bordered.empty())
            return -100;
    }

    deconv(bottom_blob, top_blob_bordered, weight_data, bias_data, opt);

    if (pad_w > 0 || pad_h > 0 || output_pad_w > 0 || output_pad_h > 0)
    {
        copy_cut_border(top_blob_bordered, top_blob, pad_h, pad_h, pad_w, pad_w, opt.blob_allocator, opt.num_threads);
        if (top_blob.empty())
            return -100;

        outw = top_blob.w;
        outh = top_blob.h;
    }
    else
    {
        top_blob = top_blob_bordered;
    }

    if (activation)
    {
        activation->forward_inplace(top_blob, opt);
    }

    return 0;
}

} // namespace ncnn
