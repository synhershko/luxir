// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/HttpReq.h"
#include "test/GrpcClient.h"
#include "test/GrpcLuxirTest.h"
#include "luxir/server/Prometheus.h"
#include "luxir/server/HttpServer.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

namespace luxir::test {
namespace {
double sample(const std::string& text, const std::string& name) {
  auto pos = text.find("\n" + name + " ");
  if (pos == std::string::npos) {
    ADD_FAILURE() << "Missing metric: " << name;
    return std::nan("");
  }
  return std::stod(text.substr(pos + name.size() + 2));
}
std::string httpLabels(std::string_view operation, std::string_view outcome) {
  return "{transport=\"http\",operation=\"" + std::string(operation) + "\",outcome=\"" + std::string(outcome) + "\"}";
}
}

class PrometheusTest : public LuxirTest {};

TEST_F(PrometheusTest, HttpTrafficAndIndexingOutcomes) {
  LuxirNode node;
  HttpServer server(node, 2, 0);
  server.start();
  auto port = server.getPort();
  auto update = httpRequest(port, http::verb::post, "/collections/metrics/_update",
      R"({"docs":[{"id":"one"},{"id":"two"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto search = httpRequest(port, http::verb::post, "/collections/metrics/_search",
      R"({"ops":{"q":{"top_docs":{"limit":1,"get_number":true}}}})");
  ASSERT_EQ(200, search.result_int()) << search.body();
  ASSERT_EQ(400, httpRequest(port, http::verb::post, "/collections/metrics/_search", "{").result_int());
  ASSERT_EQ(404, httpRequest(port, http::verb::get, "/does-not-exist").result_int());
  ASSERT_EQ(405, httpRequest(port, http::verb::post, "/health").result_int());
  auto response = httpRequest(port, http::verb::get, "/metrics");
  ASSERT_EQ(200, response.result_int());
  EXPECT_EQ("text/plain; version=0.0.4; charset=utf-8", response[http::field::content_type]);
  auto& text = response.body();
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("update", "ok")));
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("search", "ok")));
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("search", "error")));
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("health", "error")));
  EXPECT_EQ(2, sample(text, "luxir_ingest_documents_total{outcome=\"accepted\"}"));
  EXPECT_EQ(0, sample(text, "luxir_requests_in_flight{transport=\"http\",operation=\"search\"}"));
  EXPECT_EQ(0, sample(text, "luxir_metrics_collection_errors"));
  EXPECT_GT(sample(text, "luxir_background_total{kind=\"commit\",outcome=\"ok\"}"), 0);
  EXPECT_GT(sample(text, "luxir_index_committed_bytes{tenant=\"t0\",collection=\"metrics\"}"), 0);
  EXPECT_EQ(1, sample(text, "luxir_requests_duration_seconds_count{transport=\"http\",operation=\"update\"}"));
  EXPECT_EQ(1, sample(text, "luxir_requests_duration_seconds_bucket{transport=\"http\",operation=\"update\",le=\"+Inf\"}"));
  // Scraping itself must not inflate service traffic or response-body bytes.
  auto again = httpRequest(port, http::verb::get, "/metrics").body();
  EXPECT_EQ(sample(text, "luxir_response_body_bytes_total{transport=\"http\"}"),
            sample(again, "luxir_response_body_bytes_total{transport=\"http\"}"));
  server.shutdown();
  EXPECT_EQ(0, node.metrics->httpThreads.load());
}

TEST_F(PrometheusTest, StreamingFailuresAndAtomicRollback) {
  LuxirNode node;
  HttpServer server(node, 1, 0);
  server.start();
  auto port = server.getPort();
  auto schema = httpRequest(port, http::verb::post, "/collections/_create",
      R"({"name":"metrics","schema":{"fields":{"id":{"type":"id"},"n":{"type":"int"}}}})");
  ASSERT_EQ(200, schema.result_int()) << schema.body();
  auto atomic = httpRequest(port, http::verb::post, "/collections/metrics/_update",
      R"({"all_or_none":true,"docs":[{"id":"one","n":1},{"id":"bad","n":"not-a-number"}]})");
  ASSERT_EQ(200, atomic.result_int()) << atomic.body();
  auto stream = httpRequest(port, http::verb::post, "/collections/metrics/_update",
      "{\"id\":\"good\",\"n\":2}\n{\"id\":\"bad\",\"n\":\"not-a-number\"}\n", "application/x-ndjson");
  ASSERT_EQ(200, stream.result_int()) << stream.body();
  auto text = httpRequest(port, http::verb::get, "/metrics").body();
  EXPECT_EQ(1, sample(text, "luxir_ingest_documents_total{outcome=\"accepted\"}"));
  EXPECT_EQ(3, sample(text, "luxir_ingest_documents_total{outcome=\"rejected\"}"));
  EXPECT_EQ(2, sample(text, "luxir_requests_total" + httpLabels("update", "error")));
}

TEST_F(PrometheusTest, ConcurrentScrapesAndCollectionRemoval) {
  LuxirNode node;
  node.createCollection("metrics");
  HttpServer server(node, 2, 0);
  server.start();
  std::vector<std::future<std::string>> scrapes;
  for (int i = 0; i < 4; ++i) scrapes.push_back(std::async(std::launch::async, [&] {
    return httpRequest(server.getPort(), http::verb::get, "/metrics").body();
  }));
  node.deleteCollection("metrics");
  for (auto& scrape : scrapes) {
    auto text = scrape.get();
    EXPECT_NE(std::string::npos, text.find("# TYPE luxir_requests_total counter\n"));
  }
  EXPECT_EQ(std::string::npos, prometheusMetrics(node).find("collection=\"metrics\""));
}

TEST_F(PrometheusTest, EscapingHistogramsAndCollectorControls) {
  PrometheusText text;
  MetricHistogram histogram;
  histogram.observe(.001);
  histogram.observe(150);
  text.histogram("test_seconds", "Latency.", histogram,
      PrometheusText::label("label", "quote\" slash\\ newline\n"));
  auto rendered = text.str();
  EXPECT_NE(std::string::npos, rendered.find("label=\"quote\\\" slash\\\\ newline\\n\""));
  EXPECT_NE(std::string::npos, rendered.find("le=\"+Inf\"} 2\n"));
  EXPECT_NE(std::string::npos, rendered.find("le=\"0.001\"} 1\n"));
  LuxirConfig config;
  config.metrics.collections = false;
  config.metrics.system = false;
  LuxirNode node(config);
  node.createCollection("metrics");
  rendered = prometheusMetrics(node);
  EXPECT_EQ(std::string::npos, rendered.find("collection=\"metrics\""));
  EXPECT_EQ(std::string::npos, rendered.find("process_cpu_seconds_total"));
  EXPECT_EQ(node.collectionEntries().size(), sample(rendered, "luxir_collections{state=\"available\"}"));
}

#ifdef __linux__
TEST_F(PrometheusTest, RealProcessAndSystemCounters) {
  PrometheusText before;
  collectSystemMetrics(before, "/tmp");
  auto baseline = before.str();
  std::ifstream extra("/proc/self/status");
  ASSERT_TRUE(extra.good());
  std::promise<void> stop;
  auto future = stop.get_future();
  std::thread thread([&] { future.wait(); });
  PrometheusText after;
  collectSystemMetrics(after, "/tmp");
  auto text = after.str();
  stop.set_value();
  thread.join();
  EXPECT_GE(sample(text, "process_threads"), sample(baseline, "process_threads") + 1);
  EXPECT_GE(sample(text, "process_open_fds"), sample(baseline, "process_open_fds") + 1);
  for (auto name : {"process_resident_memory_bytes", "process_virtual_memory_bytes", "process_max_fds",
                    "process_start_time_seconds", "node_memory_MemTotal_bytes", "luxir_data_filesystem_size_bytes{path=\"/tmp\"}"})
    EXPECT_GT(sample(text, name), 0) << name;
  for (auto name : {"process_cpu_seconds_total", "process_disk_read_bytes_total",
                    "node_context_switches_total", "node_load1", "luxir_data_filesystem_files_free{path=\"/tmp\"}"})
    EXPECT_GE(sample(text, name), 0) << name;
  EXPECT_NE(std::string::npos, text.find("node_network_receive_bytes_total{device="));
  EXPECT_NE(std::string::npos, text.find("node_disk_read_bytes_total{device="));
  EXPECT_NE(std::string::npos, text.find("node_cpu_seconds_total{cpu="));
  PrometheusText missing;
  collectSystemMetrics(missing, "/nonexistent-luxir-metrics-directory");
  auto failed = missing.str();
  EXPECT_EQ(0, sample(failed, "luxir_metrics_collector_success{collector=\"filesystem\"}"));
  EXPECT_EQ(std::string::npos, failed.find("luxir_data_filesystem_size_bytes"));
}
#endif

class PrometheusGrpcTest : public GrpcLuxirTest {};
TEST_F(PrometheusGrpcTest, RpcCountersAndErrors) {
  auto channel = getChannel();
  auto& metric = luxirNode->metrics->request(MetricTransport::GRPC, MetricOperation::SCHEMA);
  auto errors = metric.completed[(size_t)MetricOutcome::ERROR].load();
  api::SchemaRequest request;
  request.collection = "missing-metrics-collection";
  Reply<api::SchemaResponse> reply;
  grpc::ClientContext context;
  auto status = hppUnaryCall(channel.get(), rpc::GetSchema, &context, request, &reply);
  EXPECT_FALSE(status.ok());
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (metric.completed[(size_t)MetricOutcome::ERROR].load() == errors && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(errors + 1, metric.completed[(size_t)MetricOutcome::ERROR].load());
  EXPECT_EQ(0, metric.active.load());
}
} // namespace luxir::test
