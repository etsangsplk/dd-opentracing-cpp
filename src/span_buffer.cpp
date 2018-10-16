#include "span_buffer.h"
#include <iostream>
#include "sample.h"
#include "span.h"
#include "writer.h"

namespace datadog {
namespace opentracing {

namespace {
const std::string sampling_priority_metric = "_sampling_priority_v1";
}  // namespace

void PendingTrace::finish() {
  if (finished_spans->size() == 0) {
    std::cerr << "finish called on trace with no spans" << std::endl;
    return;  // I don't know why this would ever happen.
  }
  // Check for sampling.
  if (sampling_priority != nullptr) {
    // Set the metric for every span in the trace.
    for (auto& span : *finished_spans) {
      span->metrics[sampling_priority_metric] = static_cast<int>(*sampling_priority);
    }
  }
}

WritingSpanBuffer::WritingSpanBuffer(std::shared_ptr<Writer> writer) : writer_(writer) {}

void WritingSpanBuffer::registerSpan(const SpanContext& context) {
  std::lock_guard<std::mutex> lock_guard{mutex_};
  uint64_t trace_id = context.traceId();
  auto trace = traces_.find(trace_id);
  if (trace == traces_.end()) {
    traces_.emplace(std::make_pair(trace_id, PendingTrace{}));
    trace = traces_.find(trace_id);
    auto propagation_status = context.getPropagationStatus();
    trace->second.sampling_priority_locked = propagation_status.first;
    trace->second.sampling_priority = std::move(propagation_status.second);
  }
  trace->second.all_spans.insert(context.id());
}

void WritingSpanBuffer::finishSpan(std::unique_ptr<SpanData> span,
                                   const std::shared_ptr<SampleProvider>& sampler) {
  std::lock_guard<std::mutex> lock_guard{mutex_};
  auto trace_iter = traces_.find(span->traceId());
  if (trace_iter == traces_.end()) {
    std::cerr << "Missing trace for finished span" << std::endl;
    return;
  }
  auto& trace = trace_iter->second;
  if (trace.all_spans.find(span->spanId()) == trace.all_spans.end()) {
    std::cerr << "A Span that was not registered was submitted to WritingSpanBuffer" << std::endl;
    return;
  }
  uint64_t trace_id = span->traceId();
  trace.finished_spans->push_back(std::move(span));
  if (trace.finished_spans->size() == trace.all_spans.size()) {
    assignSamplingPriorityImpl(sampler, trace.finished_spans->back().get());
    trace.finish();
    unbufferAndWriteTrace(trace_id);
  }
}

void WritingSpanBuffer::unbufferAndWriteTrace(uint64_t trace_id) {
  auto trace_iter = traces_.find(trace_id);
  if (trace_iter == traces_.end()) {
    return;
  }
  auto& trace = trace_iter->second;
  writer_->write(std::move(trace.finished_spans));
  traces_.erase(trace_iter);
}

OptionalSamplingPriority WritingSpanBuffer::getSamplingPriority(uint64_t trace_id) const {
  std::lock_guard<std::mutex> lock_guard{mutex_};
  return getSamplingPriorityImpl(trace_id);
}
OptionalSamplingPriority WritingSpanBuffer::getSamplingPriorityImpl(uint64_t trace_id) const {
  auto trace = traces_.find(trace_id);
  if (trace == traces_.end()) {
    std::cerr << "Missing trace in getSamplingPriority" << std::endl;
    return nullptr;
  }
  if (trace->second.sampling_priority == nullptr) {
    return nullptr;
  }
  return std::make_unique<SamplingPriority>(*trace->second.sampling_priority);
}

OptionalSamplingPriority WritingSpanBuffer::setSamplingPriority(
    uint64_t trace_id, OptionalSamplingPriority priority) {
  std::lock_guard<std::mutex> lock_guard{mutex_};
  return setSamplingPriorityImpl(trace_id, std::move(priority));
}

OptionalSamplingPriority WritingSpanBuffer::setSamplingPriorityImpl(
    uint64_t trace_id, OptionalSamplingPriority priority) {
  auto trace_entry = traces_.find(trace_id);
  if (trace_entry == traces_.end()) {
    std::cerr << "Missing trace in setSamplingPriority" << std::endl;
    return nullptr;
  }
  PendingTrace& trace = trace_entry->second;
  if (trace.sampling_priority_locked) {
    std::cerr << "Sampling priority locked, trace already propagated" << std::endl;
    return getSamplingPriorityImpl(trace_id);
  }
  if (priority == nullptr) {
    trace.sampling_priority.reset(nullptr);
  } else {
    trace.sampling_priority.reset(new SamplingPriority(*priority));
    if (*priority == SamplingPriority::SamplerDrop || *priority == SamplingPriority::SamplerKeep) {
      // This is an automatically-assigned sampling priority.
      trace.sampling_priority_locked = true;
    }
  }
  return getSamplingPriorityImpl(trace_id);
}

OptionalSamplingPriority WritingSpanBuffer::assignSamplingPriority(
    const std::shared_ptr<SampleProvider>& sampler, const SpanData* span) {
  std::lock_guard<std::mutex> lock{mutex_};
  return assignSamplingPriorityImpl(sampler, span);
}

OptionalSamplingPriority WritingSpanBuffer::assignSamplingPriorityImpl(
    const std::shared_ptr<SampleProvider>& sampler, const SpanData* span) {
  bool sampling_priority_unset = getSamplingPriorityImpl(span->trace_id) == nullptr;
  if (sampling_priority_unset) {
    setSamplingPriorityImpl(span->trace_id,
                            sampler->sample(span->env(), span->service, span->trace_id));
  }
  return getSamplingPriorityImpl(span->trace_id);
}

}  // namespace opentracing
}  // namespace datadog
