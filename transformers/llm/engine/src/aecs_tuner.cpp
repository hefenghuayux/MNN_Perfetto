#include "aecs_tuner.hpp"

#include <MNN/MNNDefine.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/utsname.h>
#endif

namespace MNN {
namespace Transformer {
namespace {

static double nowSeconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

static bool fileExists(const std::string& path) {
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

static bool readTextFile(const std::string& path, std::string* output) {
    if (!output) {
        return false;
    }
    std::ifstream ifs(path.c_str());
    if (!ifs.good()) {
        return false;
    }
    std::ostringstream stream;
    stream << ifs.rdbuf();
    *output = stream.str();
    return true;
}

static bool writeTextFile(const std::string& path, const std::string& content) {
    std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
    if (!ofs.good()) {
        return false;
    }
    ofs << content;
    return ofs.good();
}

static std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static bool readLongLongFile(const std::string& path, long long* output) {
    if (!output) {
        return false;
    }
    std::string content;
    if (!readTextFile(path, &content)) {
        return false;
    }
    content = trim(content);
    if (content.empty()) {
        return false;
    }
    char* end = nullptr;
    const long long value = strtoll(content.c_str(), &end, 10);
    if (end == content.c_str()) {
        return false;
    }
    *output = value;
    return true;
}

static bool runCommand(const std::string& command, std::string* output) {
    if (!output) {
        return false;
    }
    output->clear();
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }
    char buffer[512] = {0};
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output->append(buffer);
    }
    const int status = ::pclose(pipe);
    return status != -1 && !output->empty();
}

static std::vector<int> parseIntList(const std::string& content) {
    std::vector<int> values;
    std::istringstream stream(content);
    int value = 0;
    while (stream >> value) {
        values.push_back(value);
    }
    return values;
}

static double normalizeTemperature(long long raw) {
    const double abs_raw = std::fabs(static_cast<double>(raw));
    if (abs_raw >= 10000.0) {
        return static_cast<double>(raw) / 1000.0;
    }
    if (abs_raw >= 200.0) {
        return static_cast<double>(raw) / 10.0;
    }
    return static_cast<double>(raw);
}

static std::vector<int> sortedUniqueDesc(std::vector<int> values) {
    std::sort(values.begin(), values.end(), std::greater<int>());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

static std::string joinCpuIds(const std::vector<int>& cpu_ids) {
    std::ostringstream stream;
    for (size_t i = 0; i < cpu_ids.size(); ++i) {
        if (i > 0) {
            stream << ",";
        }
        stream << cpu_ids[i];
    }
    return stream.str();
}

static std::vector<std::string> listDirectories(const std::string& path, const std::string& prefix) {
    std::vector<std::string> entries;
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return entries;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string name(entry->d_name);
        if (!prefix.empty() && name.find(prefix) != 0) {
            continue;
        }
        entries.push_back(path + "/" + name);
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end());
    return entries;
}

static std::string detectDeviceFingerprint() {
#if defined(__ANDROID__)
    char buffer[PROP_VALUE_MAX + 1] = {0};
    if (__system_property_get("ro.build.fingerprint", buffer) > 0) {
        return buffer;
    }
    if (__system_property_get("ro.product.model", buffer) > 0) {
        return buffer;
    }
#endif

#if defined(__linux__) || defined(__ANDROID__)
    struct utsname uts;
    if (uname(&uts) == 0) {
        std::ostringstream stream;
        stream << uts.sysname << "-" << uts.nodename << "-" << uts.release << "-" << uts.machine;
        return stream.str();
    }
#endif
    return "unknown-device";
}

static int readCpuCapacity(int cpu_id) {
    const std::vector<std::string> candidates = {
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id) + "/cpu_capacity",
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id) + "/capacity",
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu_id) + "/cpufreq/cpuinfo_max_freq",
    };
    long long value = 0;
    for (const auto& path : candidates) {
        if (readLongLongFile(path, &value) && value > 0) {
            return static_cast<int>(value);
        }
    }
    return 0;
}

static bool containsIgnoreCase(const std::string& text, const std::string& pattern) {
    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    };
    return lower(text).find(lower(pattern)) != std::string::npos;
}

static bool parseBoolString(const std::string& text, bool* output) {
    if (!output) {
        return false;
    }
    std::string lowered = trim(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lowered == "true") {
        *output = true;
        return true;
    }
    if (lowered == "false") {
        *output = false;
        return true;
    }
    return false;
}

static bool parseKeyValueLine(const std::string& line, std::string* key, std::string* value) {
    if (!key || !value) {
        return false;
    }
    const auto pos = line.find(':');
    if (pos == std::string::npos) {
        return false;
    }
    *key = trim(line.substr(0, pos));
    *value = trim(line.substr(pos + 1));
    return !key->empty();
}

static bool parseLongLongText(const std::string& text, long long* output) {
    if (!output) {
        return false;
    }
    const std::string trimmed_text = trim(text);
    if (trimmed_text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long long value = strtoll(trimmed_text.c_str(), &end, 10);
    if (end == trimmed_text.c_str()) {
        return false;
    }
    *output = value;
    return true;
}

static bool extractFieldValue(const std::string& line, const std::string& field, std::string* value) {
    if (!value) {
        return false;
    }
    const std::string token = field + "=";
    const auto begin = line.find(token);
    if (begin == std::string::npos) {
        return false;
    }
    const auto value_begin = begin + token.size();
    auto value_end = line.find(',', value_begin);
    if (value_end == std::string::npos) {
        value_end = line.find('}', value_begin);
    }
    if (value_end == std::string::npos) {
        value_end = line.size();
    }
    *value = trim(line.substr(value_begin, value_end - value_begin));
    return !value->empty();
}

static double normalizeSysfsPower(long long raw) {
    const double abs_raw = std::fabs(static_cast<double>(raw));
    if (abs_raw >= 1000000.0) {
        return static_cast<double>(raw) / 1.0e6;
    }
    if (abs_raw >= 1000.0) {
        return static_cast<double>(raw) / 1000.0;
    }
    return static_cast<double>(raw);
}

static double normalizeSysfsCurrent(long long raw) {
    const double abs_raw = std::fabs(static_cast<double>(raw));
    if (abs_raw >= 1000000.0) {
        return static_cast<double>(raw) / 1.0e6;
    }
    if (abs_raw >= 1000.0) {
        return static_cast<double>(raw) / 1000.0;
    }
    return static_cast<double>(raw);
}

static double normalizeSysfsVoltage(long long raw) {
    const double abs_raw = std::fabs(static_cast<double>(raw));
    if (abs_raw >= 1000000.0) {
        return static_cast<double>(raw) / 1.0e6;
    }
    if (abs_raw >= 1000.0) {
        return static_cast<double>(raw) / 1000.0;
    }
    return static_cast<double>(raw);
}

struct DumpsysBatteryInfo {
    bool valid = false;
    bool voltage_valid = false;
    bool current_valid = false;
    bool charge_counter_valid = false;
    bool battery_temp_valid = false;
    bool external_power_valid = false;
    bool external_power = false;
    double voltage_v = 0.0;
    double current_a = 0.0;
    long long charge_counter_uah = 0;
    double battery_c = 0.0;
};

static bool parseDumpsysBattery(const std::string& text, DumpsysBatteryInfo* output) {
    if (!output) {
        return false;
    }
    DumpsysBatteryInfo info;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::string key;
        std::string value;
        if (!parseKeyValueLine(line, &key, &value)) {
            continue;
        }
        long long number = 0;
        if (key == "voltage" || key == "Charger voltage" || key == "Charge counter" ||
            key == "temperature" || key == "Battery current" || key == "PhoneTemp") {
            if (!parseLongLongText(value, &number)) {
                continue;
            }
        }
        if (key == "voltage") {
            info.voltage_valid = true;
            info.voltage_v = normalizeSysfsVoltage(number);
        } else if (key == "Charger voltage" && !info.voltage_valid) {
            info.voltage_valid = true;
            info.voltage_v = normalizeSysfsVoltage(number);
        } else if (key == "Charge counter") {
            info.charge_counter_valid = true;
            info.charge_counter_uah = number;
        } else if (key == "temperature") {
            info.battery_temp_valid = true;
            info.battery_c = normalizeTemperature(number);
        } else if (key == "PhoneTemp" && !info.battery_temp_valid) {
            info.battery_temp_valid = true;
            info.battery_c = normalizeTemperature(number);
        } else if (key == "Battery current") {
            info.current_valid = true;
            info.current_a = static_cast<double>(number) / 1000.0;
        } else if (key == "AC powered" || key == "USB powered" ||
                   key == "Wireless powered" || key == "Dock powered") {
            bool powered = false;
            if (parseBoolString(value, &powered)) {
                info.external_power_valid = true;
                info.external_power = info.external_power || powered;
            }
        }
    }
    info.valid = info.voltage_valid || info.current_valid || info.charge_counter_valid || info.battery_temp_valid;
    *output = info;
    return info.valid;
}

static bool sampleDumpsysBattery(DumpsysBatteryInfo* output) {
    std::string text;
    if (!runCommand("dumpsys battery", &text)) {
        return false;
    }
    return parseDumpsysBattery(text, output);
}

struct ThermalServiceInfo {
    bool cpu_valid = false;
    bool battery_valid = false;
    double cpu_c = 0.0;
    double battery_c = 0.0;
    std::string cpu_name;
};

static bool parseThermalServiceTemperatures(const std::string& text, ThermalServiceInfo* output) {
    if (!output) {
        return false;
    }
    struct Candidate {
        bool cpu_valid = false;
        bool battery_valid = false;
        double cpu_c = 0.0;
        double battery_c = 0.0;
        std::string cpu_name;
    };
    Candidate cached;
    Candidate hal;
    Candidate* active = nullptr;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (trimmed == "Cached temperatures:") {
            active = &cached;
            continue;
        }
        if (trimmed == "Current temperatures from HAL:") {
            active = &hal;
            continue;
        }
        if (active == nullptr || trimmed.find("Temperature{") != 0) {
            continue;
        }
        std::string value_text;
        std::string type_text;
        std::string name_text;
        if (!extractFieldValue(trimmed, "mValue", &value_text) ||
            !extractFieldValue(trimmed, "mType", &type_text) ||
            !extractFieldValue(trimmed, "mName", &name_text)) {
            continue;
        }
        const double value_c = atof(value_text.c_str());
        const int type = atoi(type_text.c_str());
        if (type == 0) {
            if (!active->cpu_valid || value_c > active->cpu_c) {
                active->cpu_valid = true;
                active->cpu_c = value_c;
                active->cpu_name = name_text;
            }
        } else if (type == 2 && !active->battery_valid) {
            active->battery_valid = true;
            active->battery_c = value_c;
        }
    }

    const Candidate& best = hal.cpu_valid || hal.battery_valid ? hal : cached;
    output->cpu_valid = best.cpu_valid;
    output->battery_valid = best.battery_valid;
    output->cpu_c = best.cpu_c;
    output->battery_c = best.battery_c;
    output->cpu_name = best.cpu_name;
    return output->cpu_valid || output->battery_valid;
}

static bool sampleThermalService(ThermalServiceInfo* output) {
    std::string text;
    if (!runCommand("dumpsys thermalservice", &text)) {
        return false;
    }
    return parseThermalServiceTemperatures(text, output);
}

static bool shouldIgnoreThermalZone(const std::string& zone_type, double temperature_c) {
    if (!std::isfinite(temperature_c) || temperature_c <= 0.0 || temperature_c > 200.0) {
        return true;
    }
    static const std::vector<std::string> ignored_patterns = {
        "battery",
        "bcl",
        "vbat",
        "ibat",
        "current",
        "voltage",
        "lvl",
        "charger",
    };
    for (const auto& pattern : ignored_patterns) {
        if (containsIgnoreCase(zone_type, pattern)) {
            return true;
        }
    }
    return false;
}

static ThermalSample sampleThermalState(const AecsTuningConfig& config) {
    ThermalSample sample;

    const auto thermal_dirs = listDirectories("/sys/class/thermal", "thermal_zone");
    double max_thermal_c = -std::numeric_limits<double>::infinity();
    std::string hottest_type;
    std::string thermal_source = "sysfs";
    std::string battery_source;
    for (const auto& dir : thermal_dirs) {
        std::string type_text;
        long long raw_temp = 0;
        if (!readTextFile(dir + "/type", &type_text)) {
            continue;
        }
        if (!readLongLongFile(dir + "/temp", &raw_temp)) {
            continue;
        }
        const std::string trimmed_type = trim(type_text);
        const double thermal_c = normalizeTemperature(raw_temp);
        if (shouldIgnoreThermalZone(trimmed_type, thermal_c)) {
            continue;
        }
        if (thermal_c > max_thermal_c) {
            max_thermal_c = thermal_c;
            hottest_type = trimmed_type;
        }
    }
    if (std::isfinite(max_thermal_c)) {
        sample.thermal_valid = true;
        sample.thermal_c = max_thermal_c;
    }

    const std::vector<std::string> battery_temp_paths = {
        "/sys/class/power_supply/battery/temp",
        "/sys/class/power_supply/Battery/temp",
    };
    for (const auto& path : battery_temp_paths) {
        long long raw_temp = 0;
        if (readLongLongFile(path, &raw_temp)) {
            sample.battery_valid = true;
            sample.battery_c = normalizeTemperature(raw_temp);
            battery_source = path;
            break;
        }
    }

    if (!sample.battery_valid) {
        DumpsysBatteryInfo battery_info;
        if (sampleDumpsysBattery(&battery_info) && battery_info.battery_temp_valid) {
            sample.battery_valid = true;
            sample.battery_c = battery_info.battery_c;
            battery_source = "dumpsys battery";
        }
    }

    if (!sample.thermal_valid || !sample.battery_valid) {
        ThermalServiceInfo service_info;
        if (sampleThermalService(&service_info)) {
            if (!sample.thermal_valid && service_info.cpu_valid) {
                sample.thermal_valid = true;
                sample.thermal_c = service_info.cpu_c;
                hottest_type = service_info.cpu_name;
                thermal_source = "thermalservice";
            }
            if (!sample.battery_valid && service_info.battery_valid) {
                sample.battery_valid = true;
                sample.battery_c = service_info.battery_c;
                battery_source = "thermalservice";
            }
        }
    }

    const bool thermal_hot = sample.thermal_valid && sample.thermal_c >= config.thermal_high_c;
    const bool battery_hot = sample.battery_valid && sample.battery_c >= config.battery_high_c;
    sample.overheating = thermal_hot || battery_hot;

    std::ostringstream summary;
    if (sample.thermal_valid) {
        summary << "thermal=" << sample.thermal_c << "C";
        if (!hottest_type.empty()) {
            summary << "(" << hottest_type << ")";
        }
        if (!thermal_source.empty()) {
            summary << "[" << thermal_source << "]";
        }
    } else {
        summary << "thermal=n/a";
    }
    summary << ", ";
    if (sample.battery_valid) {
        summary << "battery=" << sample.battery_c << "C";
        if (!battery_source.empty()) {
            summary << "[" << battery_source << "]";
        }
    } else {
        summary << "battery=n/a";
    }
    sample.summary = summary.str();
    return sample;
}

static bool ensureDirectoryForFile(const std::string& file_path) {
    const auto pos = file_path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return true;
    }
    const std::string dir = file_path.substr(0, pos);
    if (dir.empty()) {
        return true;
    }

    std::string current;
    for (size_t i = 0; i < dir.size(); ++i) {
        const char ch = dir[i];
        current.push_back(ch);
        if (ch != '/' && i + 1 != dir.size()) {
            continue;
        }
        if (current.empty()) {
            continue;
        }
        if (current.size() == 1 && current[0] == '/') {
            continue;
        }
        if (fileExists(current)) {
            continue;
        }
        if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }
    if (!fileExists(dir) && ::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static std::vector<int> parseCpuIdArray(const rapidjson::Value& value) {
    std::vector<int> cpu_ids;
    if (!value.IsArray()) {
        return cpu_ids;
    }
    for (auto iter = value.Begin(); iter != value.End(); ++iter) {
        if (iter->IsInt()) {
            cpu_ids.push_back(iter->GetInt());
        }
    }
    return cpu_ids;
}

static void writeCpuIdArray(rapidjson::Value* dst,
                            const std::vector<int>& cpu_ids,
                            rapidjson::Document::AllocatorType& allocator) {
    dst->SetArray();
    for (auto cpu_id : cpu_ids) {
        dst->PushBack(cpu_id, allocator);
    }
}

static rapidjson::Value toStringValue(const std::string& value,
                                      rapidjson::Document::AllocatorType& allocator) {
    rapidjson::Value json_value;
    json_value.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), allocator);
    return json_value;
}

static std::string jsonGetString(const rapidjson::Value& value,
                                 const char* key,
                                 const std::string& fallback = "") {
    if (!value.IsObject() || !value.HasMember(key) || !value[key].IsString()) {
        return fallback;
    }
    return value[key].GetString();
}

static int jsonGetInt(const rapidjson::Value& value, const char* key, int fallback = 0) {
    if (!value.IsObject() || !value.HasMember(key) || !value[key].IsInt()) {
        return fallback;
    }
    return value[key].GetInt();
}

static double jsonGetDouble(const rapidjson::Value& value, const char* key, double fallback = 0.0) {
    if (!value.IsObject() || !value.HasMember(key) || !value[key].IsNumber()) {
        return fallback;
    }
    return value[key].GetDouble();
}

static bool jsonGetBool(const rapidjson::Value& value, const char* key, bool fallback = false) {
    if (!value.IsObject() || !value.HasMember(key) || !value[key].IsBool()) {
        return fallback;
    }
    return value[key].GetBool();
}

static std::vector<int> removeCpuIdsInCluster(const std::vector<int>& cpu_ids,
                                              const std::vector<int>& cluster_cpu_ids) {
    std::vector<int> result;
    for (auto cpu_id : cpu_ids) {
        if (std::find(cluster_cpu_ids.begin(), cluster_cpu_ids.end(), cpu_id) == cluster_cpu_ids.end()) {
            result.push_back(cpu_id);
        }
    }
    return result;
}

static int maxCpuId(const std::vector<int>& cpu_ids) {
    if (cpu_ids.empty()) {
        return -1;
    }
    return *std::max_element(cpu_ids.begin(), cpu_ids.end());
}

} // namespace

AecsCpuTopology AecsCpuInspector::inspect(int preferred_prefill_start_cpu) {
    AecsCpuTopology topology;
    topology.device_fingerprint = detectDeviceFingerprint();

    std::vector<AecsClusterInfo> raw_clusters;
    const auto policy_dirs = listDirectories("/sys/devices/system/cpu/cpufreq", "policy");
    for (const auto& dir : policy_dirs) {
        std::string cpu_list_text;
        if (!readTextFile(dir + "/related_cpus", &cpu_list_text) &&
            !readTextFile(dir + "/affected_cpus", &cpu_list_text)) {
            continue;
        }
        auto cpu_ids = sortedUniqueDesc(parseIntList(cpu_list_text));
        if (cpu_ids.empty()) {
            continue;
        }

        long long min_freq = 0;
        long long max_freq = 0;
        readLongLongFile(dir + "/cpuinfo_min_freq", &min_freq);
        readLongLongFile(dir + "/cpuinfo_max_freq", &max_freq);

        AecsClusterInfo cluster;
        cluster.min_freq = static_cast<uint32_t>(std::max<long long>(0, min_freq));
        cluster.max_freq = static_cast<uint32_t>(std::max<long long>(0, max_freq));
        cluster.cpu_ids = cpu_ids;
        for (auto cpu_id : cluster.cpu_ids) {
            cluster.capacity = std::max(cluster.capacity, readCpuCapacity(cpu_id));
        }
        if (cluster.capacity <= 0) {
            cluster.capacity = static_cast<int>(cluster.max_freq);
        }
        raw_clusters.push_back(cluster);
    }

    if (raw_clusters.empty()) {
        const auto cpu_dirs = listDirectories("/sys/devices/system/cpu", "cpu");
        AecsClusterInfo cluster;
        for (const auto& dir : cpu_dirs) {
            const auto pos = dir.find_last_of("cpu");
            if (pos == std::string::npos) {
                continue;
            }
            const std::string suffix = dir.substr(dir.find_last_not_of("0123456789") + 1);
            if (suffix.empty()) {
                continue;
            }
            const int cpu_id = atoi(suffix.c_str());
            cluster.cpu_ids.push_back(cpu_id);
            long long max_freq = 0;
            if (readLongLongFile(dir + "/cpufreq/cpuinfo_max_freq", &max_freq) && max_freq > 0) {
                cluster.max_freq = std::max(cluster.max_freq, static_cast<uint32_t>(max_freq));
                if (cluster.min_freq == 0) {
                    cluster.min_freq = static_cast<uint32_t>(max_freq);
                } else {
                    cluster.min_freq = std::min(cluster.min_freq, static_cast<uint32_t>(max_freq));
                }
            }
            cluster.capacity = std::max(cluster.capacity, readCpuCapacity(cpu_id));
        }
        cluster.cpu_ids = sortedUniqueDesc(cluster.cpu_ids);
        if (!cluster.cpu_ids.empty()) {
            if (cluster.capacity <= 0) {
                cluster.capacity = static_cast<int>(cluster.max_freq);
            }
            raw_clusters.push_back(cluster);
        }
    }

    if (raw_clusters.empty()) {
        MNN_PRINT("[AECS] CPU topology unavailable, falling back to empty topology\n");
        return topology;
    }

    std::sort(raw_clusters.begin(), raw_clusters.end(), [](const AecsClusterInfo& left, const AecsClusterInfo& right) {
        const int left_max_cpu = maxCpuId(left.cpu_ids);
        const int right_max_cpu = maxCpuId(right.cpu_ids);
        if (left_max_cpu != right_max_cpu) {
            return left_max_cpu > right_max_cpu;
        }
        return left.cpu_ids.size() < right.cpu_ids.size();
    });

    topology.clusters_desc.reserve(raw_clusters.size());
    for (size_t i = 0; i < raw_clusters.size(); ++i) {
        auto cluster = raw_clusters[i];
        cluster.index = static_cast<int>(i);
        cluster.performance_rank = static_cast<int>(i);
        for (auto cpu_id : cluster.cpu_ids) {
            topology.cpu_to_cluster[cpu_id] = cluster.index;
        }
        topology.biggest_capacity = std::max(topology.biggest_capacity, cluster.capacity);
        topology.biggest_freq = std::max(topology.biggest_freq, cluster.max_freq);
        topology.clusters_desc.push_back(cluster);
    }

    for (size_t i = 0; i < topology.clusters_desc.size(); ++i) {
        auto& cluster = topology.clusters_desc[i];
        if (topology.clusters_desc.size() == 1) {
            cluster.tier = "performance";
        } else if (i == 0) {
            cluster.tier = "prime";
        } else if (i + 1 == topology.clusters_desc.size()) {
            cluster.tier = "efficient";
        } else {
            cluster.tier = "performance";
        }
        topology.all_cpu_ids_desc.insert(topology.all_cpu_ids_desc.end(), cluster.cpu_ids.begin(), cluster.cpu_ids.end());
    }

    topology.prefill_order = topology.all_cpu_ids_desc;
    if (preferred_prefill_start_cpu >= 0) {
        auto iter = std::find(topology.prefill_order.begin(), topology.prefill_order.end(), preferred_prefill_start_cpu);
        if (iter != topology.prefill_order.end()) {
            topology.prefill_order.erase(topology.prefill_order.begin(), iter);
        } else {
            topology.prefill_order.insert(topology.prefill_order.begin(), preferred_prefill_start_cpu);
        }
    }

    if (topology.clusters_desc.size() <= 1) {
        topology.decode_stage1_order = topology.all_cpu_ids_desc;
    } else {
        for (size_t i = 0; i + 1 < topology.clusters_desc.size(); ++i) {
            const auto& cluster = topology.clusters_desc[i];
            topology.decode_stage1_order.insert(topology.decode_stage1_order.end(),
                                                cluster.cpu_ids.begin(),
                                                cluster.cpu_ids.end());
        }
    }

    if (topology.decode_stage1_order.empty()) {
        topology.decode_stage1_order = topology.all_cpu_ids_desc;
    }

    MNN_PRINT("[AECS] Device=%s, prefill order=%s, decode stage1 order=%s\n",
              topology.device_fingerprint.c_str(),
              joinCpuIds(topology.prefill_order).c_str(),
              joinCpuIds(topology.decode_stage1_order).c_str());
    return topology;
}

ThermalGuard::ThermalGuard(const AecsTuningConfig& config)
    : mConfig(config) {
    mLatest = sampleThermalState(mConfig);
    mThread = std::thread(&ThermalGuard::sampleLoop, this);
}

ThermalGuard::~ThermalGuard() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStop = true;
    }
    mCondition.notify_all();
    if (mThread.joinable()) {
        mThread.join();
    }
}

ThermalSample ThermalGuard::latestSample() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mLatest;
}

void ThermalGuard::sampleLoop() {
    double last_log_time_s = -1.0;
    bool last_overheating = false;
    const auto interval = std::chrono::milliseconds(std::max(50, mConfig.thermal_sample_ms));
    while (true) {
        ThermalSample latest = sampleThermalState(mConfig);
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mStop) {
                return;
            }
            mLatest = latest;
            latest = mLatest;
        }
        mCondition.notify_all();
        const double now_s = nowSeconds();
        const double log_interval_s = std::max(5.0, static_cast<double>(interval.count()) / 1000.0);
        if (last_log_time_s < 0.0 || now_s - last_log_time_s >= log_interval_s ||
            latest.overheating != last_overheating) {
            // MNN_PRINT("[AECS][Thermal] %s status=%s\n",
            //           latest.summary.c_str(),
            //           latest.overheating ? "hot" : "cool");
            last_log_time_s = now_s;
            last_overheating = latest.overheating;
        }

        std::unique_lock<std::mutex> lock(mMutex);
        if (mCondition.wait_for(lock, interval, [this]() { return mStop; })) {
            return;
        }
    }
}

void ThermalGuard::waitUntilCool(const std::string& reason) const {
    auto cooled = [this](const ThermalSample& sample) {
        const bool thermal_cool = !sample.thermal_valid || sample.thermal_c <= mConfig.thermal_resume_c;
        const bool battery_cool = !sample.battery_valid || sample.battery_c <= mConfig.battery_resume_c;
        return thermal_cool && battery_cool;
    };

    std::unique_lock<std::mutex> lock(mMutex);
    if (!mLatest.overheating) {
        return;
    }

    const double pause_begin_s = nowSeconds();
    MNN_PRINT("[AECS][Thermal] Pause %s because %s exceeded thresholds\n",
              reason.c_str(),
              mLatest.summary.c_str());
    mCondition.wait(lock, [&]() { return mStop || cooled(mLatest); });
    if (!mStop) {
        MNN_PRINT("[AECS][Thermal] Resume %s at %s after %.2f s\n",
                  reason.c_str(),
                  mLatest.summary.c_str(),
                  std::max(0.0, nowSeconds() - pause_begin_s));
    }
}

void ThermalGuard::checkAndPause(const std::string& reason) const {
    waitUntilCool(reason);
}

EnergyProfiler::EnergyProfiler(const AecsTuningConfig& config)
    : mConfig(config) {
    std::vector<std::string> supplies = listDirectories("/sys/class/power_supply", "");
    auto battery_iter = std::find(supplies.begin(), supplies.end(), "/sys/class/power_supply/battery");
    if (battery_iter != supplies.end() && battery_iter != supplies.begin()) {
        std::rotate(supplies.begin(), battery_iter, battery_iter + 1);
    }

    auto readableValue = [](const std::string& path, long long* value) {
        return readLongLongFile(path, value);
    };

    long long probe = 0;
    for (const auto& dir : supplies) {
        const std::vector<std::string> power_candidates = {
            dir + "/power_now",
            dir + "/power_avg",
        };
        for (const auto& power_path : power_candidates) {
            if (readableValue(power_path, &probe)) {
                mSourceType = SourceType::SYSFS_POWER;
                mPowerPath = power_path;
                mAvailable = true;
                break;
            }
        }
        if (mAvailable) {
            break;
        }

        const std::vector<std::string> current_candidates = {
            dir + "/current_now",
            dir + "/current_avg",
        };
        const std::vector<std::string> voltage_candidates = {
            dir + "/voltage_now",
            dir + "/voltage_ocv",
        };
        for (const auto& current_path : current_candidates) {
            if (!readableValue(current_path, &probe)) {
                continue;
            }
            for (const auto& voltage_path : voltage_candidates) {
                if (!readableValue(voltage_path, &probe)) {
                    continue;
                }
                mSourceType = SourceType::SYSFS_CURRENT_VOLTAGE;
                mCurrentPath = current_path;
                mVoltagePath = voltage_path;
                mAvailable = true;
                break;
            }
            if (mAvailable) {
                break;
            }
        }
        if (mAvailable) {
            break;
        }
    }

    if (!mAvailable) {
        DumpsysBatteryInfo info;
        if (sampleDumpsysBattery(&info) && info.voltage_valid &&
            (info.charge_counter_valid || info.current_valid)) {
            mSourceType = SourceType::DUMPSYS_BATTERY;
            mAvailable = true;
            MNN_PRINT("[AECS][Power] Using dumpsys battery fallback: voltage=%.3f V%s%s\n",
                      info.voltage_v,
                      info.charge_counter_valid ? ", charge_counter=on" : "",
                      info.current_valid ? ", battery_current=on" : "");
            if (info.external_power_valid && info.external_power) {
                MNN_PRINT("[AECS][Power] External power is attached; dumpsys battery energy is net battery-side only and may be inaccurate\n");
            }
        }
    }

    if (!mAvailable) {
        MNN_PRINT("[AECS][Power] No accessible power source found; energy measurement disabled\n");
        return;
    }

    if (mSourceType == SourceType::SYSFS_POWER) {
        MNN_PRINT("[AECS][Power] Using direct power path=%s\n", mPowerPath.c_str());
        mThread = std::thread(&EnergyProfiler::sampleLoop, this);
    } else if (mSourceType == SourceType::SYSFS_CURRENT_VOLTAGE) {
        MNN_PRINT("[AECS][Power] Using current=%s, voltage=%s\n", mCurrentPath.c_str(), mVoltagePath.c_str());
        mThread = std::thread(&EnergyProfiler::sampleLoop, this);
    }
}

EnergyProfiler::~EnergyProfiler() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStop = true;
    }
    mCondition.notify_all();
    if (mThread.joinable()) {
        mThread.join();
    }
}

bool EnergyProfiler::available() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mAvailable;
}

void EnergyProfiler::begin() {
    if (mSourceType == SourceType::DUMPSYS_BATTERY) {
        DumpsysBatteryInfo info;
        const bool ok = sampleDumpsysBattery(&info);
        std::lock_guard<std::mutex> lock(mMutex);
        mAccumulatedEnergyJ = 0.0;
        mSampleCount = 0;
        mLastSnapshot = Snapshot();
        mMeasureBeginSnapshot = Snapshot();
        mMeasureBeginBatterySnapshot = BatterySnapshot();
        mMeasuring = mAvailable && ok;
        if (mMeasuring) {
            mMeasureBeginBatterySnapshot.valid = true;
            mMeasureBeginBatterySnapshot.timestamp_s = nowSeconds();
            mMeasureBeginBatterySnapshot.voltage_valid = info.voltage_valid;
            mMeasureBeginBatterySnapshot.current_valid = info.current_valid;
            mMeasureBeginBatterySnapshot.charge_counter_valid = info.charge_counter_valid;
            mMeasureBeginBatterySnapshot.external_power_valid = info.external_power_valid;
            mMeasureBeginBatterySnapshot.external_power = info.external_power;
            mMeasureBeginBatterySnapshot.voltage_v = info.voltage_v;
            mMeasureBeginBatterySnapshot.current_a = info.current_a;
            mMeasureBeginBatterySnapshot.charge_counter_uah = info.charge_counter_uah;
        }
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    mAccumulatedEnergyJ = 0.0;
    mSampleCount = 0;
    mLastSnapshot = Snapshot();
    mMeasureBeginSnapshot = Snapshot();
    mMeasureBeginBatterySnapshot = BatterySnapshot();
    mMeasuring = mAvailable;
    if (mMeasuring) {
        mCondition.notify_all();
    }
}

PowerSampleResult EnergyProfiler::end() {
    if (mSourceType == SourceType::DUMPSYS_BATTERY) {
        DumpsysBatteryInfo end_info;
        const bool end_ok = sampleDumpsysBattery(&end_info);
        std::lock_guard<std::mutex> lock(mMutex);
        PowerSampleResult result;
        if (mMeasuring && mMeasureBeginBatterySnapshot.valid && end_ok) {
            BatterySnapshot end_snapshot;
            end_snapshot.valid = true;
            end_snapshot.timestamp_s = nowSeconds();
            end_snapshot.voltage_valid = end_info.voltage_valid;
            end_snapshot.current_valid = end_info.current_valid;
            end_snapshot.charge_counter_valid = end_info.charge_counter_valid;
            end_snapshot.external_power_valid = end_info.external_power_valid;
            end_snapshot.external_power = end_info.external_power;
            end_snapshot.voltage_v = end_info.voltage_v;
            end_snapshot.current_a = end_info.current_a;
            end_snapshot.charge_counter_uah = end_info.charge_counter_uah;
            result.duration_s = std::max(0.0, end_snapshot.timestamp_s - mMeasureBeginBatterySnapshot.timestamp_s);

            const bool external_power = (mMeasureBeginBatterySnapshot.external_power_valid &&
                                         mMeasureBeginBatterySnapshot.external_power) ||
                                        (end_snapshot.external_power_valid && end_snapshot.external_power);
            const bool can_use_counter = mMeasureBeginBatterySnapshot.charge_counter_valid &&
                                         end_snapshot.charge_counter_valid &&
                                         mMeasureBeginBatterySnapshot.voltage_valid &&
                                         end_snapshot.voltage_valid &&
                                         mMeasureBeginBatterySnapshot.charge_counter_uah != end_snapshot.charge_counter_uah &&
                                         !external_power;
            const bool can_use_current = mMeasureBeginBatterySnapshot.current_valid &&
                                         end_snapshot.current_valid &&
                                         mMeasureBeginBatterySnapshot.voltage_valid &&
                                         end_snapshot.voltage_valid &&
                                         result.duration_s > 0.0;
            const double avg_current_a = can_use_current
                                             ? 0.5 * (std::fabs(mMeasureBeginBatterySnapshot.current_a) +
                                                      std::fabs(end_snapshot.current_a))
                                             : 0.0;
            const double avg_voltage_v = (mMeasureBeginBatterySnapshot.voltage_valid && end_snapshot.voltage_valid)
                                             ? 0.5 * (mMeasureBeginBatterySnapshot.voltage_v + end_snapshot.voltage_v)
                                             : 0.0;
            const double current_power_w = can_use_current ? avg_current_a * avg_voltage_v : 0.0;
            if (can_use_counter && result.duration_s > 0.0) {
                const long long delta_uah = end_snapshot.charge_counter_uah - mMeasureBeginBatterySnapshot.charge_counter_uah;
                const double counter_energy_j = std::fabs(static_cast<double>(delta_uah)) * avg_voltage_v * 0.0036;
                const double counter_power_w = counter_energy_j / result.duration_s;
                const bool counter_sane = !can_use_current ||
                                          (counter_power_w <= current_power_w * 4.0 + 1.0 &&
                                           counter_power_w >= std::max(0.0, current_power_w * 0.25 - 0.25));
                if (counter_sane) {
                    result.energy_j = counter_energy_j;
                    result.avg_power_w = counter_power_w;
                    result.valid = result.energy_j > 0.0;
                    result.sample_count = 2;
                }
            }
            if (!result.valid && can_use_current) {
                result.avg_power_w = current_power_w;
                result.energy_j = result.avg_power_w * result.duration_s;
                result.valid = result.energy_j > 0.0;
                result.sample_count = 2;
            }
        }
        mMeasuring = false;
        mAccumulatedEnergyJ = 0.0;
        mSampleCount = 0;
        mLastSnapshot = Snapshot();
        mMeasureBeginSnapshot = Snapshot();
        mMeasureBeginBatterySnapshot = BatterySnapshot();
        return result;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    PowerSampleResult result;
    result.valid = mMeasuring && mSampleCount > 0;
    result.energy_j = mAccumulatedEnergyJ;
    result.sample_count = mSampleCount;
    if (mMeasureBeginSnapshot.valid && mLastSnapshot.valid) {
        result.duration_s = std::max(0.0, mLastSnapshot.timestamp_s - mMeasureBeginSnapshot.timestamp_s);
    }
    if (result.valid && result.duration_s > 0.0) {
        result.avg_power_w = result.energy_j / result.duration_s;
    }
    mMeasuring = false;
    mAccumulatedEnergyJ = 0.0;
    mSampleCount = 0;
    mLastSnapshot = Snapshot();
    mMeasureBeginSnapshot = Snapshot();
    mCondition.notify_all();
    return result;
}

void EnergyProfiler::sampleLoop() {
    if (mSourceType != SourceType::SYSFS_POWER &&
        mSourceType != SourceType::SYSFS_CURRENT_VOLTAGE) {
        return;
    }
    const auto interval = std::chrono::milliseconds(std::max(10, mConfig.power_sample_ms));
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mCondition.wait(lock, [this]() { return mStop || mMeasuring; });
            if (mStop) {
                return;
            }
        }

        Snapshot snapshot;
        snapshot.timestamp_s = nowSeconds();
        long long raw_power = 0;
        long long raw_current = 0;
        long long raw_voltage = 0;
        if (mSourceType == SourceType::SYSFS_POWER && readLongLongFile(mPowerPath, &raw_power)) {
            snapshot.valid = true;
            snapshot.power_w = std::fabs(normalizeSysfsPower(raw_power));
        } else if (mSourceType == SourceType::SYSFS_CURRENT_VOLTAGE &&
                   readLongLongFile(mCurrentPath, &raw_current) &&
                   readLongLongFile(mVoltagePath, &raw_voltage)) {
            snapshot.valid = true;
            snapshot.power_w = std::fabs(normalizeSysfsCurrent(raw_current) *
                                         normalizeSysfsVoltage(raw_voltage));
        }
        if (snapshot.valid) {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mMeasuring) {
                if (!mMeasureBeginSnapshot.valid) {
                    mMeasureBeginSnapshot = snapshot;
                    mLastSnapshot = snapshot;
                } else if (mLastSnapshot.valid) {
                    const double dt = std::max(0.0, snapshot.timestamp_s - mLastSnapshot.timestamp_s);
                    mAccumulatedEnergyJ += mLastSnapshot.power_w * dt;
                    mLastSnapshot = snapshot;
                    ++mSampleCount;
                } else {
                    mLastSnapshot = snapshot;
                }
            }
        }

        std::unique_lock<std::mutex> lock(mMutex);
        if (mCondition.wait_for(lock, interval, [this]() { return mStop || !mMeasuring; }) && mStop) {
            return;
        }
    }
}

AecsTuner::AecsTuner(const AecsCpuTopology& topology,
                     const AecsTuningConfig& config,
                     const AecsHeuristicParams& heuristic_params)
    : mTopology(topology),
      mConfig(config),
      mHeuristicParams(heuristic_params) {
}

std::vector<int> AecsTuner::normalizeCpuIds(const std::vector<int>& cpu_ids) const {
    std::vector<int> unique_cpu_ids;
    std::set<int> seen;
    for (auto cpu_id : cpu_ids) {
        if (seen.insert(cpu_id).second) {
            unique_cpu_ids.push_back(cpu_id);
        }
    }

    std::vector<int> ordered;
    for (auto cpu_id : mTopology.all_cpu_ids_desc) {
        if (std::find(unique_cpu_ids.begin(), unique_cpu_ids.end(), cpu_id) != unique_cpu_ids.end()) {
            ordered.push_back(cpu_id);
        }
    }
    for (auto cpu_id : unique_cpu_ids) {
        if (std::find(ordered.begin(), ordered.end(), cpu_id) == ordered.end()) {
            ordered.push_back(cpu_id);
        }
    }
    return ordered;
}

std::vector<std::vector<int>> AecsTuner::buildPrefillCandidates() const {
    std::vector<std::vector<int>> candidates;
    std::vector<int> prefix;
    for (auto cpu_id : mTopology.prefill_order) {
        prefix.push_back(cpu_id);
        candidates.push_back(prefix);
    }
    return candidates;
}

double AecsTuner::heuristicPower(const std::vector<int>& cpu_ids) const {
    if (cpu_ids.empty() || mTopology.clusters_desc.empty()) {
        return 0.0;
    }

    std::map<int, int> selected_per_cluster;
    int selected_biggest_capacity = 0;
    uint32_t selected_biggest_freq = 0;
    for (auto cpu_id : cpu_ids) {
        const auto iter = mTopology.cpu_to_cluster.find(cpu_id);
        if (iter == mTopology.cpu_to_cluster.end()) {
            continue;
        }
        selected_per_cluster[iter->second]++;
        const auto& cluster = mTopology.clusters_desc[iter->second];
        selected_biggest_capacity = std::max(selected_biggest_capacity, cluster.capacity);
        selected_biggest_freq = std::max(selected_biggest_freq, cluster.max_freq);
    }

    const double scale_denom = (mHeuristicParams.scale_strategy == AecsClusterScaleStrategy::CAPACITY_THEN_FREQ &&
                                mTopology.biggest_capacity > 0)
                                   ? static_cast<double>(mTopology.biggest_capacity)
                                   : static_cast<double>(std::max<uint32_t>(1, mTopology.biggest_freq));
    const double scale_numer = (mHeuristicParams.scale_strategy == AecsClusterScaleStrategy::CAPACITY_THEN_FREQ &&
                                selected_biggest_capacity > 0)
                                   ? static_cast<double>(selected_biggest_capacity)
                                   : static_cast<double>(std::max<uint32_t>(1, selected_biggest_freq));
    const double selected_scale = std::max(0.0, std::min(1.0, scale_numer / scale_denom));

    double total = mHeuristicParams.static_power;
    for (const auto& cluster : mTopology.clusters_desc) {
        const int selected_count = selected_per_cluster[cluster.index];
        if (selected_count == 0 && mHeuristicParams.idle_factor <= 0.0) {
            continue;
        }
        double cluster_scale = 1.0;
        if (mHeuristicParams.scale_strategy == AecsClusterScaleStrategy::CAPACITY_THEN_FREQ &&
            mTopology.biggest_capacity > 0 && cluster.capacity > 0) {
            cluster_scale = static_cast<double>(cluster.capacity) / static_cast<double>(mTopology.biggest_capacity);
        } else if (mTopology.biggest_freq > 0) {
            cluster_scale = static_cast<double>(cluster.max_freq) / static_cast<double>(mTopology.biggest_freq);
        }
        const double selected_and_idle = static_cast<double>(selected_count) +
                                         static_cast<double>(static_cast<int>(cluster.cpu_ids.size()) - selected_count) *
                                             mHeuristicParams.idle_factor;
        const double scaled_freq_ghz =
            static_cast<double>(cluster.max_freq) * selected_scale / 1000000.0;
        total += cluster_scale * selected_and_idle * scaled_freq_ghz * scaled_freq_ghz;
    }
    return total;
}

std::vector<std::vector<int>> AecsTuner::buildDecodeStage2Candidates(const std::vector<int>& root_cpu_ids) const {
    std::vector<std::vector<int>> ordered_candidates;
    std::set<std::vector<int>> visited;
    const auto root = normalizeCpuIds(root_cpu_ids);
    if (root.empty()) {
        return ordered_candidates;
    }

    std::function<void(const std::vector<int>&, int)> expand = [&](const std::vector<int>& current, int depth) {
        if (!visited.insert(current).second) {
            return;
        }
        ordered_candidates.push_back(current);
        if (depth >= 2) {
            return;
        }

        std::vector<std::vector<int>> next_candidates;

        if (depth == 0 && current.size() >= 2) {
            std::vector<int> remove_one = current;
            remove_one.pop_back();
            next_candidates.push_back(normalizeCpuIds(remove_one));
        }
        if (depth == 0 && current.size() >= 3) {
            std::vector<int> remove_two = current;
            remove_two.pop_back();
            remove_two.pop_back();
            next_candidates.push_back(normalizeCpuIds(remove_two));
        }

        std::map<int, std::vector<int>> selected_by_cluster;
        std::map<int, std::vector<int>> unselected_by_cluster;
        for (const auto& cluster : mTopology.clusters_desc) {
            for (auto cpu_id : cluster.cpu_ids) {
                if (std::find(current.begin(), current.end(), cpu_id) != current.end()) {
                    selected_by_cluster[cluster.index].push_back(cpu_id);
                } else {
                    unselected_by_cluster[cluster.index].push_back(cpu_id);
                }
            }
        }

        for (size_t high_idx = 0; high_idx < mTopology.clusters_desc.size(); ++high_idx) {
            const auto& high_cluster = mTopology.clusters_desc[high_idx];
            if (selected_by_cluster[high_cluster.index].empty()) {
                continue;
            }
            for (size_t low_idx = high_idx + 1; low_idx < mTopology.clusters_desc.size(); ++low_idx) {
                const auto& low_cluster = mTopology.clusters_desc[low_idx];
                if (!selected_by_cluster[low_cluster.index].empty() &&
                    !unselected_by_cluster[low_cluster.index].empty()) {
                    std::vector<int> candidate = current;
                    const int removed_cpu = selected_by_cluster[high_cluster.index].back();
                    candidate.erase(std::remove(candidate.begin(), candidate.end(), removed_cpu), candidate.end());
                    candidate.push_back(unselected_by_cluster[low_cluster.index].front());
                    next_candidates.push_back(normalizeCpuIds(candidate));
                }
                if (selected_by_cluster[low_cluster.index].empty() &&
                    !unselected_by_cluster[low_cluster.index].empty()) {
                    std::vector<int> candidate =
                        removeCpuIdsInCluster(current, selected_by_cluster[high_cluster.index]);
                    const size_t replace_count =
                        std::min(selected_by_cluster[high_cluster.index].size(),
                                 unselected_by_cluster[low_cluster.index].size());
                    for (size_t i = 0; i < replace_count; ++i) {
                        candidate.push_back(unselected_by_cluster[low_cluster.index][i]);
                    }
                    next_candidates.push_back(normalizeCpuIds(candidate));
                }
            }
        }

        for (const auto& next : next_candidates) {
            if (next.empty()) {
                continue;
            }
            expand(next, depth + 1);
        }
    };

    expand(root, 0);
    return ordered_candidates;
}

AecsCandidateResult AecsTuner::tunePrefill(const PrefillMeasureFn& prefill_measure) const {
    AecsCandidateResult best;
    const auto candidates = buildPrefillCandidates();
    if (candidates.empty()) {
        return best;
    }

    for (const auto& candidate_cpu_ids : candidates) {
        AecsCandidateResult candidate;
        candidate.cpu_ids = normalizeCpuIds(candidate_cpu_ids);
        candidate.threads = static_cast<int>(candidate.cpu_ids.size());
        candidate.measurement = prefill_measure(candidate.cpu_ids, candidate.threads);
        candidate.source = "prefill_search";
        MNN_PRINT("[AECS][Prefill] candidate=%s speed=%.3f tok/s time=%.6f s\n",
                  joinCpuIds(candidate.cpu_ids).c_str(),
                  candidate.measurement.speed_tok_s,
                  candidate.measurement.time_s);

        if (best.cpu_ids.empty()) {
            best = candidate;
            continue;
        }

        const double improvement =
            best.measurement.speed_tok_s > 0.0
                ? (candidate.measurement.speed_tok_s - best.measurement.speed_tok_s) /
                      best.measurement.speed_tok_s
                : 1.0;
        if (improvement <= mConfig.prefill_stop_gain) {
            MNN_PRINT("[AECS][Prefill] stop at candidate=%s because improvement %.4f <= %.4f\n",
                      joinCpuIds(candidate.cpu_ids).c_str(),
                      improvement,
                      mConfig.prefill_stop_gain);
            return best;
        }
        best = candidate;
    }
    return best;
}

AecsCandidateResult AecsTuner::tuneDecodeStage1(const std::vector<int>& prefill_cpu_ids,
                                                int prefill_threads,
                                                const DecodeMeasureFn& decode_measure) const {
    AecsCandidateResult best;
    std::vector<int> prefix;
    for (auto cpu_id : mTopology.decode_stage1_order) {
        prefix.push_back(cpu_id);
        AecsCandidateResult candidate;
        candidate.cpu_ids = normalizeCpuIds(prefix);
        candidate.threads = static_cast<int>(candidate.cpu_ids.size());
        candidate.measurement = decode_measure(prefill_cpu_ids, prefill_threads, candidate.cpu_ids, candidate.threads);
        candidate.source = "decode_stage1";
        MNN_PRINT("[AECS][Decode][Stage1] candidate=%s speed=%.3f tok/s time=%.6f s\n",
                  joinCpuIds(candidate.cpu_ids).c_str(),
                  candidate.measurement.speed_tok_s,
                  candidate.measurement.time_s);

        if (best.cpu_ids.empty()) {
            best = candidate;
            continue;
        }

        if (candidate.measurement.speed_tok_s <= best.measurement.speed_tok_s) {
            MNN_PRINT("[AECS][Decode][Stage1] stop at candidate=%s because speed no longer improves\n",
                      joinCpuIds(candidate.cpu_ids).c_str());
            return best;
        }
        best = candidate;
    }
    return best;
}

AecsCandidateResult AecsTuner::tuneDecodeStage2(const std::vector<int>& prefill_cpu_ids,
                                                int prefill_threads,
                                                const AecsCandidateResult& fastest,
                                                std::vector<AecsCandidateResult>* all_candidates,
                                                const DecodeMeasureFn& decode_measure) const {
    if (all_candidates) {
        all_candidates->clear();
    }
    if (fastest.cpu_ids.empty()) {
        return fastest;
    }

    const auto candidates = buildDecodeStage2Candidates(fastest.cpu_ids);
    const double speed_floor = fastest.measurement.speed_tok_s * (1.0 - mConfig.speed_relaxation);
    AecsCandidateResult best = fastest;
    double best_objective = std::numeric_limits<double>::infinity();

    for (size_t i = 0; i < candidates.size(); ++i) {
        AecsCandidateResult candidate;
        candidate.cpu_ids = normalizeCpuIds(candidates[i]);
        candidate.threads = static_cast<int>(candidate.cpu_ids.size());
        if (i == 0) {
            candidate.measurement = fastest.measurement;
        } else {
            candidate.measurement = decode_measure(prefill_cpu_ids,
                                                   prefill_threads,
                                                   candidate.cpu_ids,
                                                   candidate.threads);
        }
        candidate.source = (i == 0) ? "decode_stage2_root" : "decode_stage2";
        candidate.heuristic_power = heuristicPower(candidate.cpu_ids);
        candidate.heuristic_energy = candidate.heuristic_power * candidate.measurement.time_s;
        candidate.feasible = candidate.measurement.speed_tok_s >= speed_floor;
        candidate.objective = candidate.measurement.energy_valid
                                  ? (1.0 - mHeuristicParams.alpha) * candidate.measurement.energy_j +
                                        mHeuristicParams.alpha * candidate.heuristic_energy
                                  : candidate.heuristic_energy;

        MNN_PRINT("[AECS][Decode][Stage2] candidate=%s speed=%.3f tok/s energy=%s%.6f J heuristic=%.6f objective=%.6f feasible=%d\n",
                  joinCpuIds(candidate.cpu_ids).c_str(),
                  candidate.measurement.speed_tok_s,
                  candidate.measurement.energy_valid ? "" : "(heuristic-only) ",
                  candidate.measurement.energy_valid ? candidate.measurement.energy_j : candidate.heuristic_energy,
                  candidate.heuristic_energy,
                  candidate.objective,
                  candidate.feasible ? 1 : 0);

        if (all_candidates) {
            all_candidates->push_back(candidate);
        }

        if (!candidate.feasible) {
            continue;
        }
        if (candidate.objective < best_objective) {
            best = candidate;
            best_objective = candidate.objective;
        }
    }

    if (!std::isfinite(best_objective)) {
        MNN_PRINT("[AECS][Decode][Stage2] no feasible candidate remained, fallback to fastest candidate\n");
        return fastest;
    }
    return best;
}

bool AecsTuner::matchesCacheEntry(const AecsCacheKey& cache_key,
                                  const AecsTuningConfig& cached_config,
                                  const AecsHeuristicParams& cached_heuristic,
                                  const AecsCacheKey& cached_key) const {
    const auto nearly_equal = [](double left, double right) {
        return std::fabs(left - right) <= 1e-9;
    };

    return cache_key.device_fingerprint == cached_key.device_fingerprint &&
           cache_key.model_path == cached_key.model_path &&
           cache_key.mnn_version == cached_key.mnn_version &&
           cache_key.backend == cached_key.backend &&
           cache_key.precision == cached_key.precision &&
           cache_key.memory == cached_key.memory &&
           cache_key.power == cached_key.power &&
           cache_key.dynamic_option == cached_key.dynamic_option &&
           cache_key.use_mmap == cached_key.use_mmap &&
           cache_key.n_prompt == cached_key.n_prompt &&
           cached_config.prefill_start_cpu == mConfig.prefill_start_cpu &&
           cached_config.decode_search_tokens == mConfig.decode_search_tokens &&
           nearly_equal(cached_config.prefill_stop_gain, mConfig.prefill_stop_gain) &&
           nearly_equal(cached_config.speed_relaxation, mConfig.speed_relaxation) &&
           nearly_equal(cached_heuristic.alpha, mHeuristicParams.alpha) &&
           nearly_equal(cached_heuristic.idle_factor, mHeuristicParams.idle_factor) &&
           nearly_equal(cached_heuristic.static_power, mHeuristicParams.static_power) &&
           cached_heuristic.scale_strategy == mHeuristicParams.scale_strategy;
}

bool AecsTuner::loadCache(const AecsCacheKey& cache_key, PhaseTuningResult* result) const {
    if (!result || mConfig.cache_file.empty() || !fileExists(mConfig.cache_file)) {
        return false;
    }

    std::string json_text;
    if (!readTextFile(mConfig.cache_file, &json_text)) {
        return false;
    }

    rapidjson::Document doc;
    doc.Parse(json_text.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("entries") || !doc["entries"].IsArray()) {
        return false;
    }

    for (auto iter = doc["entries"].Begin(); iter != doc["entries"].End(); ++iter) {
        if (!iter->IsObject() || !iter->HasMember("key") || !iter->HasMember("config") ||
            !iter->HasMember("heuristic") || !iter->HasMember("result")) {
            continue;
        }
        const auto& key = (*iter)["key"];
        const auto& config = (*iter)["config"];
        const auto& heuristic = (*iter)["heuristic"];
        const auto& saved_result = (*iter)["result"];

        AecsCacheKey cached_key;
        cached_key.device_fingerprint = jsonGetString(key, "device_fingerprint");
        cached_key.model_path = jsonGetString(key, "model_path");
        cached_key.mnn_version = jsonGetString(key, "mnn_version");
        cached_key.backend = jsonGetInt(key, "backend");
        cached_key.precision = jsonGetInt(key, "precision");
        cached_key.memory = jsonGetInt(key, "memory");
        cached_key.power = jsonGetInt(key, "power");
        cached_key.dynamic_option = jsonGetInt(key, "dynamic_option");
        cached_key.use_mmap = jsonGetBool(key, "use_mmap");
        cached_key.n_prompt = jsonGetInt(key, "n_prompt");

        AecsTuningConfig cached_config;
        cached_config.prefill_start_cpu = jsonGetInt(config, "prefill_start_cpu", -1);
        cached_config.prefill_stop_gain = jsonGetDouble(config, "prefill_stop_gain", 0.01);
        cached_config.decode_search_tokens = jsonGetInt(config, "decode_search_tokens", 50);
        cached_config.speed_relaxation = jsonGetDouble(config, "speed_relaxation", 0.08);

        AecsHeuristicParams cached_heuristic;
        cached_heuristic.alpha = jsonGetDouble(heuristic, "alpha", 0.5);
        cached_heuristic.idle_factor = jsonGetDouble(heuristic, "idle_factor", 0.35);
        cached_heuristic.static_power = jsonGetDouble(heuristic, "static_power", 0.15);
        cached_heuristic.scale_strategy = static_cast<AecsClusterScaleStrategy>(
            jsonGetInt(heuristic, "scale_strategy", static_cast<int>(AecsClusterScaleStrategy::CAPACITY_THEN_FREQ)));

        if (!matchesCacheEntry(cache_key, cached_config, cached_heuristic, cached_key)) {
            continue;
        }

        if (saved_result.HasMember("prefill_cpu_ids")) {
            result->prefill_cpu_ids = parseCpuIdArray(saved_result["prefill_cpu_ids"]);
        }
        result->prefill_threads = jsonGetInt(saved_result, "prefill_threads", static_cast<int>(result->prefill_cpu_ids.size()));
        if (saved_result.HasMember("decode_cpu_ids")) {
            result->decode_cpu_ids = parseCpuIdArray(saved_result["decode_cpu_ids"]);
        }
        result->decode_threads = jsonGetInt(saved_result, "decode_threads", static_cast<int>(result->decode_cpu_ids.size()));
        result->cache_hit = true;
        result->prefill_from_cache = !result->prefill_cpu_ids.empty();
        result->decode_from_cache = !result->decode_cpu_ids.empty();
        MNN_PRINT("[AECS] Cache hit for model=%s, prefill=%s, decode=%s\n",
                  cache_key.model_path.c_str(),
                  joinCpuIds(result->prefill_cpu_ids).c_str(),
                  joinCpuIds(result->decode_cpu_ids).c_str());
        return true;
    }
    return false;
}

void AecsTuner::saveCache(const AecsCacheKey& cache_key, const PhaseTuningResult& result) const {
    if (mConfig.cache_file.empty()) {
        return;
    }
    ensureDirectoryForFile(mConfig.cache_file);

    rapidjson::Document doc;
    std::string json_text;
    if (readTextFile(mConfig.cache_file, &json_text)) {
        doc.Parse(json_text.c_str());
    }
    if (doc.HasParseError() || !doc.IsObject()) {
        doc.SetObject();
    }

    auto& allocator = doc.GetAllocator();
    if (!doc.HasMember("entries") || !doc["entries"].IsArray()) {
        rapidjson::Value entries;
        entries.SetArray();
        doc.RemoveAllMembers();
        doc.AddMember("entries", entries, allocator);
    }

    rapidjson::Value entry(rapidjson::kObjectType);
    rapidjson::Value key(rapidjson::kObjectType);
    key.AddMember("device_fingerprint", toStringValue(cache_key.device_fingerprint, allocator), allocator);
    key.AddMember("model_path", toStringValue(cache_key.model_path, allocator), allocator);
    key.AddMember("mnn_version", toStringValue(cache_key.mnn_version, allocator), allocator);
    key.AddMember("backend", cache_key.backend, allocator);
    key.AddMember("precision", cache_key.precision, allocator);
    key.AddMember("memory", cache_key.memory, allocator);
    key.AddMember("power", cache_key.power, allocator);
    key.AddMember("dynamic_option", cache_key.dynamic_option, allocator);
    key.AddMember("use_mmap", cache_key.use_mmap, allocator);
    key.AddMember("n_prompt", cache_key.n_prompt, allocator);
    entry.AddMember("key", key, allocator);

    rapidjson::Value config(rapidjson::kObjectType);
    config.AddMember("prefill_start_cpu", mConfig.prefill_start_cpu, allocator);
    config.AddMember("prefill_stop_gain", mConfig.prefill_stop_gain, allocator);
    config.AddMember("decode_search_tokens", mConfig.decode_search_tokens, allocator);
    config.AddMember("speed_relaxation", mConfig.speed_relaxation, allocator);
    entry.AddMember("config", config, allocator);

    rapidjson::Value heuristic(rapidjson::kObjectType);
    heuristic.AddMember("alpha", mHeuristicParams.alpha, allocator);
    heuristic.AddMember("idle_factor", mHeuristicParams.idle_factor, allocator);
    heuristic.AddMember("static_power", mHeuristicParams.static_power, allocator);
    heuristic.AddMember("scale_strategy", static_cast<int>(mHeuristicParams.scale_strategy), allocator);
    entry.AddMember("heuristic", heuristic, allocator);

    rapidjson::Value result_json(rapidjson::kObjectType);
    rapidjson::Value prefill_ids;
    writeCpuIdArray(&prefill_ids, result.prefill_cpu_ids, allocator);
    result_json.AddMember("prefill_cpu_ids", prefill_ids, allocator);
    result_json.AddMember("prefill_threads", result.prefill_threads, allocator);
    rapidjson::Value decode_ids;
    writeCpuIdArray(&decode_ids, result.decode_cpu_ids, allocator);
    result_json.AddMember("decode_cpu_ids", decode_ids, allocator);
    result_json.AddMember("decode_threads", result.decode_threads, allocator);
    entry.AddMember("result", result_json, allocator);

    auto& entries = doc["entries"];
    for (auto iter = entries.Begin(); iter != entries.End(); ++iter) {
        if (!iter->IsObject() || !iter->HasMember("key") || !(*iter)["key"].IsObject()) {
            continue;
        }
        const auto& existing_key = (*iter)["key"];
        if (jsonGetString(existing_key, "device_fingerprint") == cache_key.device_fingerprint &&
            jsonGetString(existing_key, "model_path") == cache_key.model_path &&
            jsonGetString(existing_key, "mnn_version") == cache_key.mnn_version &&
            jsonGetInt(existing_key, "backend") == cache_key.backend &&
            jsonGetInt(existing_key, "precision") == cache_key.precision &&
            jsonGetInt(existing_key, "memory") == cache_key.memory &&
            jsonGetInt(existing_key, "power") == cache_key.power &&
            jsonGetInt(existing_key, "dynamic_option") == cache_key.dynamic_option &&
            jsonGetBool(existing_key, "use_mmap") == cache_key.use_mmap &&
            jsonGetInt(existing_key, "n_prompt") == cache_key.n_prompt) {
            *iter = entry;
            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            writeTextFile(mConfig.cache_file, buffer.GetString());
            return;
        }
    }

    entries.PushBack(entry, allocator);
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    writeTextFile(mConfig.cache_file, buffer.GetString());
}

PhaseTuningResult AecsTuner::tune(const AecsCacheKey& cache_key,
                                  const std::vector<int>& manual_prefill_cpu_ids,
                                  const std::vector<int>& manual_decode_cpu_ids,
                                  int fallback_prefill_threads,
                                  int fallback_decode_threads,
                                  const PrefillMeasureFn& prefill_measure,
                                  const DecodeMeasureFn& decode_measure) const {
    PhaseTuningResult result;
    PhaseTuningResult cached_result;
    const bool cache_loaded = !mConfig.force_retune && loadCache(cache_key, &cached_result);
    if (cache_loaded) {
        result = cached_result;
    }

    if (!manual_prefill_cpu_ids.empty()) {
        result.prefill_cpu_ids = normalizeCpuIds(manual_prefill_cpu_ids);
        result.prefill_threads = fallback_prefill_threads > 0 ? fallback_prefill_threads
                                                              : static_cast<int>(result.prefill_cpu_ids.size());
        result.prefill_from_cache = false;
    } else if (mConfig.prefill_auto_bind && result.prefill_cpu_ids.empty()) {
        const auto best_prefill = tunePrefill(prefill_measure);
        result.prefill_cpu_ids = best_prefill.cpu_ids;
        result.prefill_threads = best_prefill.threads;
        result.prefill_from_cache = false;
    }

    if (result.prefill_threads <= 0 && !result.prefill_cpu_ids.empty()) {
        result.prefill_threads = static_cast<int>(result.prefill_cpu_ids.size());
    }
    if (result.prefill_threads <= 0) {
        result.prefill_threads = fallback_prefill_threads;
    }

    if (!manual_decode_cpu_ids.empty()) {
        result.decode_cpu_ids = normalizeCpuIds(manual_decode_cpu_ids);
        result.decode_threads = fallback_decode_threads > 0 ? fallback_decode_threads
                                                            : static_cast<int>(result.decode_cpu_ids.size());
        result.decode_from_cache = false;
    } else if (mConfig.decode_aecs && result.decode_cpu_ids.empty()) {
        const auto fastest = tuneDecodeStage1(result.prefill_cpu_ids,
                                              std::max(1, result.prefill_threads),
                                              decode_measure);
        result.fastest_decode_candidate = fastest;
        result.selected_decode_candidate = tuneDecodeStage2(result.prefill_cpu_ids,
                                                            std::max(1, result.prefill_threads),
                                                            fastest,
                                                            &result.decode_candidates,
                                                            decode_measure);
        result.decode_cpu_ids = result.selected_decode_candidate.cpu_ids;
        result.decode_threads = result.selected_decode_candidate.threads;
        result.decode_from_cache = false;
    }

    if (result.decode_threads <= 0 && !result.decode_cpu_ids.empty()) {
        result.decode_threads = static_cast<int>(result.decode_cpu_ids.size());
    }
    if (result.decode_threads <= 0) {
        result.decode_threads = fallback_decode_threads;
    }

    PhaseTuningResult cache_result = result;
    if (!manual_prefill_cpu_ids.empty()) {
        cache_result.prefill_cpu_ids.clear();
        cache_result.prefill_threads = 0;
    }
    if (!manual_decode_cpu_ids.empty()) {
        cache_result.decode_cpu_ids.clear();
        cache_result.decode_threads = 0;
    }
    if ((mConfig.prefill_auto_bind || mConfig.decode_aecs) &&
        (!cache_result.prefill_cpu_ids.empty() || !cache_result.decode_cpu_ids.empty()) &&
        (!cache_loaded || !result.prefill_from_cache || !result.decode_from_cache)) {
        saveCache(cache_key, cache_result);
    }
    return result;
}

} // namespace Transformer
} // namespace MNN
