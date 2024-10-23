/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/
#pragma once

#include <miopen/convolution.hpp>
#include <miopen/logger.hpp>

namespace miopen {
namespace unit_tests {

struct ConvolutionDescriptorParams
{
    ConvolutionDescriptorParams(std::vector<int>&& pads_in,
                                std::vector<int>&& strides_in,
                                std::vector<int>&& dilations_in)
        : pads(std::move(pads_in)),
          strides(std::move(strides_in)),
          dilations(std::move(dilations_in))
    {
    }

    std::size_t GetNumSpatialDims() const { return pads.size(); }

    miopen::ConvolutionDescriptor GetConvolutionDescriptor() const
    {
        const auto trans_output_pads = std::vector<int>(pads.size(), 0);
        return {pads, strides, dilations, trans_output_pads};
    }

    friend std::ostream& operator<<(std::ostream& os, const ConvolutionDescriptorParams& cp)
    {
        LogRange(os << "{", cp.pads, ",") << "}, ";
        LogRange(os << "{", cp.strides, ",") << "}, ";
        LogRange(os << "{", cp.dilations, ",") << "}";
        return os;
    }

private:
    std::vector<int> pads;
    std::vector<int> strides;
    std::vector<int> dilations;
};

} // namespace unit_tests
} // namespace miopen