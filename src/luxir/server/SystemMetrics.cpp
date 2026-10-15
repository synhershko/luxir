// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "Prometheus.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef __linux__
#include <dirent.h>
#include <mntent.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace luxir {
namespace {
using Labels = std::string;

std::ifstream input(const std::filesystem::path& path) {
  std::ifstream in(path);
  in.imbue(std::locale::classic());
  if (!in) throw std::runtime_error("cannot read system counter");
  return in;
}

std::map<std::string, double> values(const std::filesystem::path& path) {
  auto in = input(path);
  std::map<std::string, double> result;
  std::string line, key;
  double value;
  while (std::getline(in, line)) {
    std::istringstream row(line);
    row.imbue(std::locale::classic());
    if (row >> key >> value) {
      if (key.ends_with(':')) key.pop_back();
      result[key] = value;
    }
  }
  return result;
}

void gauge(PrometheusText& out, std::string_view name, std::string_view help,
           double value, std::string_view labels = {}) {
  out.sample(name, help, "gauge", value, labels);
}
void counter(PrometheusText& out, std::string_view name, std::string_view help,
             double value, std::string_view labels = {}) {
  out.sample(name, help, "counter", value, labels);
}

#ifdef __linux__
void process(PrometheusText& out) {
  auto status = values("/proc/self/status");
  for (auto [key, name] : {
      std::pair{"VmRSS", "process_resident_memory_bytes"},
      {"VmSize", "process_virtual_memory_bytes"}, {"VmSwap", "process_swap_memory_bytes"},
      {"RssAnon", "process_resident_anonymous_bytes"}, {"RssFile", "process_resident_file_bytes"},
      {"RssShmem", "process_resident_shared_bytes"}, {"VmHWM", "process_resident_memory_peak_bytes"}}) {
    if (status.contains(key)) gauge(out, name, "Process memory from procfs in bytes.", status.at(key) * 1024);
  }
  gauge(out, "process_threads", "Current process threads.", status.at("Threads"));
  // getrusage includes all threads, unlike context-switch fields in task status.
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage failed");
  counter(out, "process_cpu_seconds_total", "CPU time used by all process threads.",
      usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6);
  counter(out, "process_context_switches_total", "Process context switches.", usage.ru_nvcsw, "kind=\"voluntary\"");
  counter(out, "process_context_switches_total", "Process context switches.", usage.ru_nivcsw, "kind=\"involuntary\"");
  counter(out, "process_page_faults_total", "Process page faults.", usage.ru_minflt, "kind=\"minor\"");
  counter(out, "process_page_faults_total", "Process page faults.", usage.ru_majflt, "kind=\"major\"");
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit)) throw std::runtime_error("getrlimit failed");
  gauge(out, "process_max_fds", "Soft file descriptor limit; -1 means unlimited.",
      limit.rlim_cur == RLIM_INFINITY ? -1 : (double)limit.rlim_cur);
  auto* directory = opendir("/proc/self/fd");
  if (!directory) throw std::runtime_error("cannot list descriptors");
  uint64_t count = 0;
  while (auto* entry = readdir(directory)) if (entry->d_name[0] != '.') ++count;
  closedir(directory);
  gauge(out, "process_open_fds", "Open descriptors, excluding the collector directory descriptor.", count - 1);

  auto stat = input("/proc/self/stat");
  std::string line;
  std::getline(stat, line);
  auto end = line.rfind(')'); // comm can contain spaces and parentheses.
  if (end == std::string::npos) throw std::runtime_error("invalid process stat");
  std::istringstream fields(line.substr(end + 2));
  std::string field;
  for (int i = 3; i <= 22; ++i) if (!(fields >> field)) throw std::runtime_error("short process stat");
  auto host = values("/proc/stat");
  gauge(out, "process_start_time_seconds", "Process start time since Unix epoch.",
      host.at("btime") + std::stod(field) / sysconf(_SC_CLK_TCK));
}

void processIO(PrometheusText& out) {
  auto io = values("/proc/self/io");
  for (auto [key, name] : {
      std::pair{"read_bytes", "process_disk_read_bytes_total"},
      {"write_bytes", "process_disk_written_bytes_total"},
      {"rchar", "process_io_read_bytes_total"}, {"wchar", "process_io_written_bytes_total"},
      {"syscr", "process_io_read_calls_total"}, {"syscw", "process_io_write_calls_total"}})
    counter(out, name, "Process I/O accounting from procfs.", io.at(key));
}

void hostCPU(PrometheusText& out) {
  auto in = input("/proc/stat");
  std::string line, key;
  constexpr std::array modes{"user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal"};
  while (std::getline(in, line)) {
    std::istringstream row(line);
    row >> key;
    if (key.starts_with("cpu") && key != "cpu") {
      for (auto mode : modes) {
        uint64_t ticks;
        if (!(row >> ticks)) throw std::runtime_error("invalid cpu stat");
        counter(out, "node_cpu_seconds_total", "CPU time by logical CPU and mode; user includes guest time.",
            (double)ticks / sysconf(_SC_CLK_TCK), PrometheusText::label("cpu", key.substr(3)) + "," + PrometheusText::label("mode", mode));
      }
    } else if (key == "ctxt" || key == "processes" || key == "procs_running" || key == "procs_blocked" || key == "btime") {
      double value;
      if (!(row >> value)) throw std::runtime_error("invalid host stat");
      if (key == "ctxt") counter(out, "node_context_switches_total", "Host context switches.", value);
      if (key == "processes") counter(out, "node_forks_total", "Host tasks created.", value);
      if (key == "procs_running") gauge(out, "node_procs_running", "Runnable tasks.", value);
      if (key == "procs_blocked") gauge(out, "node_procs_blocked", "Tasks blocked on I/O.", value);
      if (key == "btime") gauge(out, "node_boot_time_seconds", "Host boot time since Unix epoch.", value);
    }
  }
  auto load = input("/proc/loadavg");
  for (auto name : {"node_load1", "node_load5", "node_load15"}) {
    double value;
    if (!(load >> value)) throw std::runtime_error("invalid loadavg");
    gauge(out, name, "Host load average.", value);
  }
  auto files = input("/proc/sys/fs/file-nr");
  uint64_t allocated, unused, maximum;
  if (!(files >> allocated >> unused >> maximum)) throw std::runtime_error("invalid file-nr");
  gauge(out, "node_filefd_allocated", "Host allocated file handles.", allocated);
  gauge(out, "node_filefd_maximum", "Host file handle limit.", maximum);
}

void hostMemory(PrometheusText& out) {
  auto memory = values("/proc/meminfo");
  for (auto key : {"MemTotal", "MemFree", "MemAvailable", "Buffers", "Cached", "SwapCached",
                  "SwapTotal", "SwapFree", "Dirty", "Writeback", "Mapped", "Shmem", "Slab",
                  "SReclaimable", "SUnreclaim", "PageTables", "KernelStack", "AnonPages"}) {
    if (memory.contains(key)) gauge(out, std::string("node_memory_") + key + "_bytes",
        "Host memory from procfs in bytes.", memory.at(key) * 1024);
  }
  auto vm = values("/proc/vmstat");
  for (auto key : {"pgfault", "pgmajfault", "pswpin", "pswpout", "oom_kill"})
    if (vm.contains(key)) counter(out, std::string("node_vmstat_") + key + "_total", "Kernel VM event count (swap in pages).", vm.at(key));
}

void network(PrometheusText& out) {
  auto in = input("/proc/net/dev");
  std::string line;
  while (std::getline(in, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto device = line.substr(0, colon);
    device.erase(0, device.find_first_not_of(" \t"));
    std::istringstream row(line.substr(colon + 1));
    std::array<uint64_t, 16> v{};
    for (auto& value : v) if (!(row >> value)) throw std::runtime_error("invalid network stat");
    auto labels = PrometheusText::label("device", device);
    constexpr std::array names{"receive_bytes", "receive_packets", "receive_errs", "receive_drop",
        "receive_fifo", "receive_frame", "receive_compressed", "receive_multicast",
        "transmit_bytes", "transmit_packets", "transmit_errs", "transmit_drop",
        "transmit_fifo", "transmit_colls", "transmit_carrier", "transmit_compressed"};
    for (size_t i = 0; i < v.size(); ++i) counter(out, std::string("node_network_") + names[i] + "_total",
        "Network namespace interface counter.", v[i], labels);
  }
  auto snmp = input("/proc/net/snmp");
  std::string header;
  while (std::getline(snmp, header) && std::getline(snmp, line)) {
    if (!header.starts_with("Tcp:")) continue;
    std::istringstream keys(header), vals(line);
    std::string key, group;
    keys >> group; vals >> group;
    double value;
    while (keys >> key && vals >> value) {
      if (key == "CurrEstab") gauge(out, "node_netstat_Tcp_CurrEstab", "Established TCP connections in the network namespace.", value);
      if (key == "RetransSegs" || key == "InErrs" || key == "OutRsts" || key == "AttemptFails")
        counter(out, "node_netstat_Tcp_" + key + "_total", "TCP namespace event counter.", value);
    }
  }
}

void disks(PrometheusText& out) {
  auto in = input("/proc/diskstats");
  std::string line, device;
  unsigned major, minor;
  while (std::getline(in, line)) {
    std::istringstream row(line);
    std::array<uint64_t, 11> v{};
    if (!(row >> major >> minor >> device)) continue;
    for (auto& value : v) if (!(row >> value)) throw std::runtime_error("invalid disk stat");
    auto labels = PrometheusText::label("device", device);
    counter(out, "node_disk_reads_completed_total", "Completed disk reads.", v[0], labels);
    counter(out, "node_disk_reads_merged_total", "Merged disk reads.", v[1], labels);
    counter(out, "node_disk_read_bytes_total", "Disk bytes read (kernel sectors are always 512 bytes).", (double)v[2] * 512, labels);
    counter(out, "node_disk_read_time_seconds_total", "Cumulative disk read time.", v[3] / 1000.0, labels);
    counter(out, "node_disk_writes_completed_total", "Completed disk writes.", v[4], labels);
    counter(out, "node_disk_writes_merged_total", "Merged disk writes.", v[5], labels);
    counter(out, "node_disk_written_bytes_total", "Disk bytes written.", (double)v[6] * 512, labels);
    counter(out, "node_disk_write_time_seconds_total", "Cumulative disk write time.", v[7] / 1000.0, labels);
    gauge(out, "node_disk_io_now", "Disk requests in progress.", v[8], labels);
    counter(out, "node_disk_io_time_seconds_total", "Time disk had I/O in progress.", v[9] / 1000.0, labels);
    counter(out, "node_disk_io_time_weighted_seconds_total", "Weighted time spent doing I/O.", v[10] / 1000.0, labels);
  }
}

void filesystem(PrometheusText& out, std::string_view directory) {
  // Only the data filesystem: statvfs on arbitrary remote mounts can hang a
  // scrape indefinitely. Disk I/O above still covers every visible device.
  if (directory.empty()) return;
  struct statvfs stat{};
  std::string path(directory);
  bool ok = statvfs(path.c_str(), &stat) == 0;
  auto labels = PrometheusText::label("path", directory);
  gauge(out, "luxir_data_filesystem_error", "Could not stat the data filesystem.", !ok, labels);
  if (!ok) throw std::runtime_error("statvfs failed");
  gauge(out, "luxir_data_filesystem_size_bytes", "Data filesystem capacity.", (double)stat.f_blocks * stat.f_frsize, labels);
  gauge(out, "luxir_data_filesystem_free_bytes", "Data filesystem free bytes including reserved blocks.", (double)stat.f_bfree * stat.f_frsize, labels);
  gauge(out, "luxir_data_filesystem_avail_bytes", "Data filesystem bytes available to this process.", (double)stat.f_bavail * stat.f_frsize, labels);
  gauge(out, "luxir_data_filesystem_files", "Data filesystem inode capacity.", stat.f_files, labels);
  gauge(out, "luxir_data_filesystem_files_free", "Data filesystem free inodes.", stat.f_ffree, labels);
  gauge(out, "luxir_data_filesystem_readonly", "Data filesystem is mounted read only.", (stat.f_flag & ST_RDONLY) != 0, labels);
}

void pressure(PrometheusText& out) {
  for (auto resource : {"cpu", "memory", "io"}) {
    auto in = input(std::string("/proc/pressure/") + resource);
    std::string line, kind, field;
    while (std::getline(in, line)) {
      std::istringstream row(line);
      row >> kind;
      while (row >> field) if (field.starts_with("total="))
        counter(out, "node_pressure_stall_seconds_total", "Time stalled on a resource (PSI some/full).",
            std::stod(field.substr(6)) / 1e6,
            PrometheusText::label("resource", resource) + "," + PrometheusText::label("kind", kind));
    }
  }
}

// Match the current cgroup to its mount root, including cgroup namespaces and
// bind mounts. Never blindly report the host root as this process's cgroup.
std::filesystem::path cgroupDirectory() {
  auto membership = input("/proc/self/cgroup");
  std::string line, group;
  while (std::getline(membership, line)) if (line.starts_with("0::")) group = line.substr(3);
  if (group.empty()) throw std::runtime_error("cgroup v2 unavailable");
  auto mounts = input("/proc/self/mountinfo");
  while (std::getline(mounts, line)) {
    if (line.find(" - cgroup2 ") == std::string::npos) continue;
    std::istringstream row(line);
    std::string id, parent, device, root, mount;
    row >> id >> parent >> device >> root >> mount;
    // Mountinfo encodes whitespace and backslashes as octal escapes.
    auto unescape = [](std::string s) {
      for (size_t p = 0; p + 3 < s.size(); ++p) if (s[p] == '\\' &&
          s[p+1] >= '0' && s[p+1] <= '7' && s[p+2] >= '0' && s[p+2] <= '7' && s[p+3] >= '0' && s[p+3] <= '7') {
        char c = (char)((s[p+1]-'0')*64 + (s[p+2]-'0')*8 + s[p+3]-'0');
        s.replace(p, 4, 1, c);
      }
      return s;
    };
    root = unescape(root); mount = unescape(mount);
    if (root == "/" || group == root || group.starts_with(root + "/")) {
      std::string relative = root == "/" ? group.substr(1) : group.substr(root.size());
      while (relative.starts_with('/')) relative.erase(0, 1);
      auto path = (std::filesystem::path(mount) / relative).lexically_normal();
      auto rel = path.lexically_relative(mount);
      if (rel.empty() || *rel.begin() == "..") continue;
      return path;
    }
  }
  throw std::runtime_error("cgroup v2 mount unavailable");
}

void cgroupScalar(PrometheusText& out, const std::filesystem::path& path,
                  const char* file, const char* name, const char* help) {
  auto in = input(path / file);
  std::string value;
  if (!(in >> value)) throw std::runtime_error("invalid cgroup scalar");
  gauge(out, name, help, value == "max" ? -1 : std::stod(value));
}
void cgroupMemory(PrometheusText& out, const std::filesystem::path& path) {
  cgroupScalar(out, path, "memory.current", "luxir_cgroup_memory_current_bytes", "Cgroup memory including page cache and descendants.");
  cgroupScalar(out, path, "memory.max", "luxir_cgroup_memory_max_bytes", "Local cgroup memory limit; -1 means unlimited. Ancestors may impose stricter limits.");
  cgroupScalar(out, path, "memory.high", "luxir_cgroup_memory_high_bytes", "Local cgroup memory throttle threshold; -1 means unlimited.");
  for (auto [key, value] : values(path / "memory.events"))
    if (key == "low" || key == "high" || key == "max" || key == "oom" || key == "oom_kill")
      counter(out, "luxir_cgroup_memory_events_total", "Cgroup memory events.", value, PrometheusText::label("event", key));
}
void cgroupTasks(PrometheusText& out, const std::filesystem::path& path) {
  cgroupScalar(out, path, "pids.current", "luxir_cgroup_tasks", "Cgroup task count (including threads).");
  cgroupScalar(out, path, "pids.max", "luxir_cgroup_tasks_max", "Local cgroup task limit; -1 means unlimited.");
}
void cgroupCPU(PrometheusText& out, const std::filesystem::path& path) {
  for (auto [key, value] : values(path / "cpu.stat")) {
    if (key == "usage_usec") counter(out, "luxir_cgroup_cpu_seconds_total", "Cgroup CPU time.", value / 1e6);
    if (key == "throttled_usec") counter(out, "luxir_cgroup_cpu_throttled_seconds_total", "Cgroup CPU throttling time.", value / 1e6);
    if (key == "nr_periods") counter(out, "luxir_cgroup_cpu_periods_total", "Cgroup CPU scheduling periods.", value);
    if (key == "nr_throttled") counter(out, "luxir_cgroup_cpu_throttled_periods_total", "Throttled CPU periods.", value);
  }
  auto cpu = input(path / "cpu.max");
  std::string quota;
  double period;
  if (!(cpu >> quota >> period) || period <= 0) throw std::runtime_error("invalid cpu.max");
  gauge(out, "luxir_cgroup_cpu_quota_cores", "Local CPU quota in cores; -1 means unlimited. Ancestors may impose stricter limits.",
      quota == "max" ? -1 : std::stod(quota) / period);
}

#endif
} // namespace

void collectSystemMetrics(PrometheusText& out, std::string_view dataDirectory) {
#ifdef __linux__
  auto collect = [&](const char* name, auto&& fn) {
    bool success = true;
    try { fn(); } catch (const std::exception&) { success = false; }
    gauge(out, "luxir_metrics_collector_success", "Whether the collector could read its sources; unavailable data is omitted.",
        success, PrometheusText::label("collector", name));
  };
  collect("process", [&] { process(out); });
  collect("process_io", [&] { processIO(out); });
  collect("cpu", [&] { hostCPU(out); });
  collect("memory", [&] { hostMemory(out); });
  collect("network", [&] { network(out); });
  collect("disk", [&] { disks(out); });
  collect("filesystem", [&] { filesystem(out, dataDirectory); });
  collect("pressure", [&] { pressure(out); });
  // Controllers are independent: a missing pids controller must not hide
  // otherwise available CPU and memory measurements.
  std::filesystem::path cgroupPath;
  collect("cgroup", [&] { cgroupPath = cgroupDirectory(); });
  for (auto [name, collector] : {std::pair{"cgroup_memory", cgroupMemory},
      {"cgroup_cpu", cgroupCPU}, {"cgroup_tasks", cgroupTasks}}) {
    collect(name, [&] {
      if (cgroupPath.empty()) throw std::runtime_error("cgroup unavailable");
      collector(out, cgroupPath);
    });
  }
#else
  (void)dataDirectory;
  gauge(out, "luxir_metrics_collector_success", "System collectors require Linux.", 0, "collector=\"system\"");
#endif
}
} // namespace luxir
