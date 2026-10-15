// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <map>
#include <string>
#include <string_view>
#include "luxir/util/Metrics.h"

namespace luxir {
class LuxirNode;

// Groups families so HELP/TYPE appear once, before all samples of a family.
class PrometheusText {
  struct Family { std::string help, type, samples; };
  std::map<std::string, Family, std::less<>> families;
public:
  static std::string label(std::string_view key, std::string_view value);
  void sample(std::string_view name, std::string_view help, std::string_view type,
              double value, std::string_view labels = {});
  void histogram(std::string_view name, std::string_view help,
                 const MetricHistogram& histogram, std::string_view labels = {});
  std::string str() const;
};

void collectSystemMetrics(PrometheusText& text, std::string_view dataDirectory);
std::string prometheusMetrics(LuxirNode& node);
} // namespace luxir
