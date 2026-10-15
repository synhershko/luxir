// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "Prometheus.h"
#include "LuxirNode.h"
#include "luxir/util/ProcessAllocator.h"
#include "luxir/query/QueryStats.h"
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace luxir {
namespace {
std::string number(double value) {
  if (std::isinf(value)) return value > 0 ? "+Inf" : "-Inf";
  if (std::isnan(value)) return "NaN";
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) throw std::runtime_error("metric conversion failed");
  return {buffer, result.ptr};
}
std::string line(std::string_view name, double value, std::string_view labels) {
  return std::string(name) + (labels.empty() ? "" : "{" + std::string(labels) + "}") + " " + number(value) + "\n";
}
void activity(PrometheusText& out, std::string_view prefix, const MetricActivity& metric, std::string_view labels = {}) {
  auto name = std::string(prefix);
  out.sample(name + "_in_flight", "Operations currently in flight.", "gauge", metric.active.load(), labels);
  for (size_t i = 0; i < metricOutcomes.size(); ++i) {
    std::string dims(labels);
    if (!dims.empty()) dims += ',';
    dims += PrometheusText::label("outcome", metricOutcomes[i]);
    out.sample(name + "_total", "Completed operations by outcome.", "counter", metric.completed[i].load(), dims);
  }
  out.histogram(name + "_duration_seconds", "Operation duration in seconds, including failed operations.", metric.duration, labels);
}
}

std::string PrometheusText::label(std::string_view key, std::string_view value) {
  std::string result(key);
  result += "=\"";
  for (char c : value) {
    if (c == '\\' || c == '"') result += '\\';
    if (c == '\n') result += "\\n";
    else result += c;
  }
  result += '"';
  return result;
}
void PrometheusText::sample(std::string_view name, std::string_view help, std::string_view type,
                            double value, std::string_view labels) {
  auto [it, inserted] = families.try_emplace(std::string(name), Family{std::string(help), std::string(type), {}});
  if (!inserted && it->second.type != type) throw std::logic_error("conflicting metric types");
  it->second.samples += line(name, value, labels);
}
void PrometheusText::histogram(std::string_view name, std::string_view help,
                              const MetricHistogram& histogram, std::string_view labels) {
  auto snapshot = histogram.snapshot();
  auto [it, inserted] = families.try_emplace(std::string(name), Family{std::string(help), "histogram", {}});
  if (!inserted && it->second.type != "histogram") throw std::logic_error("conflicting histogram type");
  uint64_t count = 0;
  for (size_t i = 0; i < snapshot.buckets.size(); ++i) {
    count += snapshot.buckets[i];
    std::string dims(labels);
    if (!dims.empty()) dims += ',';
    dims += label("le", i == MetricHistogram::bounds.size() ? "+Inf" : number(MetricHistogram::bounds[i]));
    it->second.samples += line(std::string(name) + "_bucket", count, dims);
  }
  it->second.samples += line(std::string(name) + "_sum", snapshot.sum, labels);
  it->second.samples += line(std::string(name) + "_count", count, labels);
}
std::string PrometheusText::str() const {
  std::string result;
  for (const auto& [name, family] : families) {
    result += "# HELP " + name + " " + family.help + "\n# TYPE " + name + " " + family.type + "\n" + family.samples;
  }
  return result;
}

std::string prometheusMetrics(LuxirNode& node) {
  auto start = std::chrono::steady_clock::now();
  PrometheusText out;
  auto gauge = [&](std::string_view name, std::string_view help, double value, std::string_view labels = {}) {
    out.sample(name, help, "gauge", value, labels);
  };
  auto counter = [&](std::string_view name, std::string_view help, double value, std::string_view labels = {}) {
    out.sample(name, help, "counter", value, labels);
  };
  auto& metrics = *node.metrics;
  for (size_t t = 0; t < 2; ++t) {
    auto transport = PrometheusText::label("transport", t == 0 ? "http" : "grpc");
    for (size_t op = 0; op < metricOperations.size(); ++op)
      activity(out, "luxir_requests", metrics.requests[t][op], transport + "," + PrometheusText::label("operation", metricOperations[op]));
    for (size_t kind = 0; kind < metrics.errors[t].size(); ++kind)
      counter("luxir_request_errors_total", "First application error per HTTP request or gRPC call, including in-band errors.",
          metrics.errors[t][kind].load(), transport + "," + PrometheusText::label("kind", errorKindName((ErrorKind)kind)));
    counter("luxir_received_body_bytes_total", "Received application payload bytes, excluding transport framing.", metrics.receivedBytes[t].load(), transport);
    counter("luxir_response_body_bytes_total", "Application payload bytes submitted for sending, including failed deliveries.", metrics.sentBytes[t].load(), transport);
    counter("luxir_stream_pauses_total", "Response producers paused by flow control.", metrics.streamPauses[t].load(), transport);
  }
  activity(out, "luxir_search", metrics.searches);
  activity(out, "luxir_index_batches", metrics.updates);
  activity(out, "luxir_background", metrics.flushes, "kind=\"flush\"");
  activity(out, "luxir_background", metrics.commits, "kind=\"commit\"");
  activity(out, "luxir_background", metrics.merges, "kind=\"merge\"");
  activity(out, "luxir_background", metrics.vectorBuilds, "kind=\"vector_build\"");
  activity(out, "luxir_background", metrics.readerRefreshes, "kind=\"reader_refresh\"");
  out.histogram("luxir_search_execution_duration_seconds", "Search body execution, excluding scheduler waits and later streaming drains.", metrics.searchExecution);
  counter("luxir_search_resource_exhausted_total", "Searches rejected by resource limits, including the request memory breaker.", metrics.searchResourceExhausted.load());
  counter("luxir_search_warnings_total", "Searches completing with at least one declared degradation warning.", metrics.searchWarnings.load());
  counter("luxir_ingest_documents_total", "Documents accepted into indexing or rejected/rolled back; acceptance is not durable acknowledgment.", metrics.documentsAccepted.load(), "outcome=\"accepted\"");
  counter("luxir_ingest_documents_total", "Documents accepted into indexing or rejected/rolled back; acceptance is not durable acknowledgment.", metrics.documentsRejected.load(), "outcome=\"rejected\"");
  counter("luxir_ingest_delete_ids_total", "Accepted delete IDs, not the number of matching documents removed.", metrics.deletesAccepted.load());
  gauge("luxir_http_connections", "Live HTTP sessions, including idle keep-alive connections.", metrics.httpConnections.load());
  gauge("luxir_http_io_threads", "Currently running HTTP connection shard threads (excludes accept thread).", metrics.httpThreads.load());
  gauge("luxir_search_request_memory_limit_bytes", "Per-request tracked search memory ceiling; zero disables rejection.", node.getConfig().search.request_memory_max_bytes);
  gauge("luxir_node_memory_budget_bytes", "Configured budget for managed memory; not an RSS ceiling. Zero means unlimited.", node.getConfig().max_ram_mb * 1024 * 1024);
  gauge("luxir_node_read_only", "Node is configured read only.", node.readOnly());
#ifdef LUXIR_METRICS_VERSION
  gauge("luxir_build_info", "Luxir build version.", 1, PrometheusText::label("version", LUXIR_METRICS_VERSION));
#endif
  auto& ram = node.getIndexRamBudget();
  gauge("luxir_index_ram_reserved_bytes", "Indexing RAM reservations, which may temporarily exceed the soft cap.", ram.reservedBytes());
  gauge("luxir_index_ram_budget_bytes", "Shared indexing RAM cap; zero means unlimited.", ram.totalBytes());
  gauge("luxir_index_ram_draining_bytes", "Reserved indexing RAM currently draining through flushes.", ram.drainingBytesCount());
  gauge("luxir_index_ram_pending_merge_bytes", "Pending merge demand, capped at the indexing RAM budget when limited.", ram.pendingMergeDemandBytes());
  gauge("luxir_storage_ram_bytes", "RAM storage bytes owned by the node; not disk usage.", node.storageBytes());
  gauge("luxir_storage_ram_limit_bytes", "RAM storage cap; zero means unlimited or not applicable.",
      node.getConfig().store.backend == "ram" ? node.getConfig().store.ram_limit_mb * 1024 * 1024 : 0);
  if (auto allocator = processAllocatorStats()) {
    for (auto [kind, value] : {std::pair{"allocated", allocator->allocated}, {"active", allocator->active},
        {"metadata", allocator->metadata}, {"resident", allocator->resident}, {"mapped", allocator->mapped},
        {"retained", allocator->retained}, {"dirty", allocator->dirty}})
      gauge("luxir_allocator_bytes", "Jemalloc accounting; resident is an allocator estimate, not process RSS.", value, PrometheusText::label("kind", kind));
    for (auto [name, arena] : {std::pair{"indexing", allocator->indexing}, {"big_buffer", allocator->bigBuffer}}) {
      gauge("luxir_allocator_arena_allocated_bytes", "Live arena allocations.", arena.allocated, PrometheusText::label("arena", name));
      gauge("luxir_allocator_arena_resident_bytes", "Allocator estimate of resident arena memory.", arena.resident, PrometheusText::label("arena", name));
    }
    gauge("luxir_allocator_stats_available", "Whether jemalloc statistics are available.", 1);
  } else gauge("luxir_allocator_stats_available", "Whether jemalloc statistics are available.", 0);

  uint64_t available = 0, unavailable = 0, failures = 0;
  auto entries = node.collectionEntries();
  for (const auto& entry : entries) {
    bool ok = entry.error.empty() && entry.collection && entry.collection->getUnavailableReason().empty();
    if (ok) ++available; else ++unavailable;
    if (!node.getConfig().metrics.collections) continue;
    auto labels = PrometheusText::label("tenant", entry.id.tenant) + "," + PrometheusText::label("collection", entry.id.name);
    gauge("luxir_collection_available", "Collection is available for requests.", ok, labels);
    if (!ok) continue;
    try {
      auto shard = entry.collection->getShard();
      auto& readers = shard->getReaderManager();
      auto snapshot = readers.snapshot();
      if (!snapshot) continue;
      uint64_t committedBytes = 0;
      for (const auto& file : snapshot->files) committedBytes += file.size;
      gauge("luxir_index_committed_bytes", "Bytes in the current snapshot file descriptors; excludes temporary and retained files.", committedBytes, labels);
      gauge("luxir_index_last_commit_timestamp_seconds", "Last published commit time since Unix epoch; age alone does not imply stale data.", snapshot->commitTime / 1e6, labels);
      auto pins = shard->getSnapshots().stats(false);
      gauge("luxir_snapshot_pins", "Current snapshot pins.", pins.pins, labels);
      gauge("luxir_snapshot_retained_bytes", "Retired bytes held by snapshot pins.", pins.retainedBytes, labels);
      auto emit = [&](const auto& stats) {
        gauge("luxir_index_documents", "Documents in flushed index segments; excludes documents still in inverters.", stats.liveDocs, labels + ",state=\"live\"");
        gauge("luxir_index_documents", "Documents in flushed index segments; excludes documents still in inverters.", stats.maxDocs - stats.liveDocs, labels + ",state=\"deleted\"");
        gauge("luxir_index_segments", "Current flushed index segments.", stats.segments, labels);
        gauge("luxir_index_uncommitted_segments", "Flushed segments not in the last published snapshot.", stats.segments - stats.committedSegments, labels);
        if constexpr (requires { stats.activeMerges; }) {
          gauge("luxir_merge_tasks", "Outstanding merge tasks, including queued work.", stats.activeMerges, labels);
          gauge("luxir_inverters", "Indexing inverters by state.", stats.flushing, labels + ",state=\"flushing\"");
          gauge("luxir_inverters", "Indexing inverters by state.", stats.indexing, labels + ",state=\"busy\"");
          gauge("luxir_inverters", "Indexing inverters by state.", stats.idle, labels + ",state=\"idle\"");
        }
        gauge("luxir_filter_cache_resident_bytes", "Resident filter-cache payload bytes.", stats.filterCacheResidentBytes, labels);
        gauge("luxir_filter_cache_metadata_bytes", "Filter-cache metadata bytes.", stats.filterCacheMetadataBytes, labels);
        gauge("luxir_filter_cache_budget_bytes", "Filter-cache payload budget; zero disables caching.", stats.filterCacheMaxBytes, labels);
        const auto& c = stats.filterCacheCounters;
        counter("luxir_filter_cache_lookups_total", "Shared filter-cache lookups; reset by cache-control reset_counters.", c.hits, labels + ",result=\"hit\"");
        counter("luxir_filter_cache_lookups_total", "Shared filter-cache lookups; reset by cache-control reset_counters.", c.misses, labels + ",result=\"miss\"");
        for (auto [name, value] : {std::pair{"builds", c.builds}, {"evictions", c.evictions}, {"purges", c.purges},
            {"admissions", c.admissions}, {"publish_rejects", c.publishRejects}, {"oversized_key_bypasses", c.oversizedKeyBypasses},
            {"build_attempts", c.buildAttempts}, {"byproduct_inserts", c.byproductInserts},
            {"dead_builds", c.capacityDeadBuilds}, {"thrash_skips", c.thrashBuildSkips},
            {"reader_dead_builds", c.readerDeadBuilds}, {"reader_thrash_skips", c.readerThrashBuildSkips},
            {"reader_stable_hits", c.readerStableHits}, {"reader_stable_refreshes", c.readerStableRefreshes},
            {"reader_stable_retires", c.readerStableRetires}})
          counter(std::string("luxir_filter_cache_") + name + "_total", "Filter-cache events; reset by cache-control reset_counters.", value, labels);
      };
      if (auto writer = shard->getIndexWriter()) emit(writer->stats(false, false));
      else emit(readers.stats(false, false));
    } catch (const std::exception&) {
      // Deletion/closure can race the owning entry snapshot. Keep other
      // collections and all process metrics available when one row fails.
      ++failures;
    }
  }
  gauge("luxir_collections", "Collections by availability.", available, "state=\"available\"");
  gauge("luxir_collections", "Collections by availability.", unavailable, "state=\"unavailable\"");
  gauge("luxir_metrics_collection_errors", "Collections whose metrics could not be read in this scrape.", failures);
  counter("luxir_multiterm_expansion_refills_total", "Process-wide multi-term expansion refills.", multitermExpansionRefills.load());
  if (node.getConfig().metrics.system) collectSystemMetrics(out, node.getConfig().store.backend == "fs" ? node.getConfig().store.data_dir : "");
  gauge("luxir_metrics_collection_duration_seconds", "Time collecting this scrape, excluding text serialization and transmission.",
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  return out.str();
}
} // namespace luxir
