/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/service/gpu/model/fusion_analysis_cache.h"

#include "xla/hlo/ir/hlo_instruction.h"

namespace xla::gpu {

const std::optional<HloFusionAnalysis>& HloFusionAnalysisCache::Get(
    const HloInstruction& instruction) {
  {
    absl::ReaderMutexLock lock(&mutex_);
    auto it = analyses_.find(&instruction);
    if (it != analyses_.end()) {
      return it->second;
    }
  }

  std::optional<HloFusionAnalysis> analysis =
      AnalyzeFusion(instruction, device_info_);
  absl::MutexLock lock(&mutex_);

  // If some other thread created an entry for this key concurrently, return
  // that instead (the other thread is likely using the instance).
  auto it = analyses_.find(&instruction);
  if (it != analyses_.end()) {
    return it->second;
  }

  return analyses_[&instruction] = std::move(analysis);
}

const std::optional<HloFusionAnalysis>& HloFusionAnalysisCache::Get(
    const HloInstruction& producer, const HloInstruction& consumer) {
  std::pair<const HloInstruction*, const HloInstruction*> key{&producer,
                                                              &consumer};
  {
    absl::ReaderMutexLock lock(&mutex_);
    auto it = producer_consumer_analyses_.find(key);
    if (it != producer_consumer_analyses_.end()) {
      return it->second;
    }
  }

  std::optional<HloFusionAnalysis> analysis =
      AnalyzeProducerConsumerFusion(producer, consumer, device_info_);
  absl::MutexLock lock(&mutex_);

  // If some other thread created an entry for this key concurrently, return
  // that instead (the other thread is likely using the instance).
  auto it = producer_consumer_analyses_.find(key);
  if (it != producer_consumer_analyses_.end()) {
    return it->second;
  }

  producers_for_consumers_[&consumer].push_back(&producer);
  consumers_for_producers_[&producer].push_back(&consumer);
  return producer_consumer_analyses_[key] = std::move(analysis);
}

void HloFusionAnalysisCache::Invalidate(const HloInstruction& instruction) {
  absl::MutexLock lock(&mutex_);
  analyses_.erase(&instruction);

  if (auto consumers = consumers_for_producers_.extract(&instruction)) {
    for (const auto* consumer : consumers.mapped()) {
      producer_consumer_analyses_.erase({&instruction, consumer});
    }
  }
  if (auto producers = producers_for_consumers_.extract(&instruction)) {
    for (const auto* producer : producers.mapped()) {
      producer_consumer_analyses_.erase({producer, &instruction});
    }
  }
}

}  // namespace xla::gpu