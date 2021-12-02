/*
 * Copyright (c) 2020-2021, NVIDIA CORPORATION.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *//*
 */

/** @file   batched.h
 *  @author Thomas Müller, NVIDIA
 *  @brief  Implementation of an optimizer wrapper that increases the effective batch size by averaging gradients over multiple training steps.
 */

#pragma once

#include <tiny-cuda-nn/common.h>
#include <tiny-cuda-nn/gpu_memory.h>
#include <tiny-cuda-nn/misc_kernels.h>
#include <tiny-cuda-nn/optimizer.h>

#include <memory>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <vector>


TCNN_NAMESPACE_BEGIN

template <typename T>
__global__ void gradient_update(
	const uint32_t n_elements,
	bool first,
	uint32_t batch_size_multiplier,
	const T* __restrict__ gradients,
	float* __restrict__ pool
) {
	const uint32_t i = threadIdx.x + blockIdx.x * blockDim.x;
	if (i >= n_elements) return;

	if (first) {
		pool[i] = 0;
	}

	pool[i] += (float)gradients[i] / batch_size_multiplier;
}

template <typename T>
class BatchedOptimizer : public Optimizer<T> {
public:
	BatchedOptimizer(const json& params) {
		m_nested.reset(create_optimizer<T>(params.value("nested", json::object())));
		update_hyperparams(params);
	}

	void allocate(std::shared_ptr<ParametricObject<T>> target) override {
		m_nested->allocate(target);

		uint32_t size = (uint32_t)target->n_params();

		m_averaged_gradients.resize(size);
		m_averaged_gradients_half.resize(size);
	}

	void step(cudaStream_t stream, float loss_scale, float learning_rate, float* weights_full_precision, T* weights, const T* gradients) override {
		linear_kernel(gradient_update<T>, 0, stream, m_nested->n_weights(), m_current_step % m_batch_size_multiplier == 0, m_batch_size_multiplier, gradients, m_averaged_gradients.data());
		++m_current_step;

		if (m_current_step % m_batch_size_multiplier == 0) {
			if (!std::is_same<T, float>::value) {
				linear_kernel(cast<T>, 0, stream, m_nested->n_weights(), m_averaged_gradients.data(), m_averaged_gradients_half.data());
			}

			m_nested->step(stream, loss_scale, learning_rate, weights_full_precision, weights, std::is_same<T, float>::value ? (T*)m_averaged_gradients.data() : (T*)m_averaged_gradients_half.data());
		}
	}

	float learning_rate() const {
		return m_nested->learning_rate();
	}

	uint32_t step() const {
		return m_current_step;
	}

	uint32_t n_weights() const {
		return m_nested->n_weights();
	}

	T* custom_weights() const {
		return m_nested->custom_weights();
	}

	void update_hyperparams(const json& params) override {
		if (params.contains("batch_size_multiplier")) {
			m_batch_size_multiplier = params["batch_size_multiplier"];
		}

		if (params.contains("nested")) {
			m_nested->update_hyperparams(params["nested"]);
		}
	}

	json serialize() const override {
		json data;
		data["nested"] = m_nested->serialize();
		data["averaged_gradients_binary"] = gpu_memory_to_json_binary(m_averaged_gradients);
		data["averaged_gradients_half_binary"] = gpu_memory_to_json_binary(m_averaged_gradients_half);
		data["current_step"] = m_current_step;
		return data;
	}

	void deserialize(const json& data) override {
		m_current_step = data["current_step"];
		json_binary_to_gpu_memory(data["averaged_gradients_binary"], m_averaged_gradients);
		json_binary_to_gpu_memory(data["averaged_gradients_half_binary"], m_averaged_gradients_half);
		m_nested->deserialize(data["nested"]);
	}

private:
	uint32_t m_batch_size_multiplier = 16;
	std::unique_ptr<Optimizer<T>> m_nested;

	GPUMemory<float> m_averaged_gradients;
	GPUMemory<T> m_averaged_gradients_half;

	uint32_t m_current_step = 0;
};

TCNN_NAMESPACE_END