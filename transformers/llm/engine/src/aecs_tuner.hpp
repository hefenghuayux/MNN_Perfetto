#ifndef LLM_AECS_TUNER_HPP
#define LLM_AECS_TUNER_HPP

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <MNN/MNNDefine.h>

namespace MNN {
namespace Transformer {

enum class AecsClusterScaleStrategy : int {
    CAPACITY_THEN_FREQ = 0,
    NORMALIZED_MAX_FREQ = 1,
};

struct AecsHeuristicParams {
    double alpha = 0.5;
    double idle_factor = 0.35;
    double static_power = 0.15;
    AecsClusterScaleStrategy scale_strategy = AecsClusterScaleStrategy::CAPACITY_THEN_FREQ;
};

struct AecsTuningConfig {
    bool prefill_auto_bind = false;
    bool decode_aecs = false;
    bool force_retune = false;
    std::string cache_file = "tmp/aecs_cache.json";
    int prefill_start_cpu = -1;
    double prefill_stop_gain = 0.01;
    int decode_search_tokens = 128;
    double speed_relaxation = 0.08;
    int thermal_sample_ms = 500;
    int power_sample_ms = 50;
    double thermal_high_c = 75.0;
    double thermal_resume_c = 70.0;
    double battery_high_c = 43.0;
    double battery_resume_c = 41.0;
    int warmup_runs = 1;
    int measure_runs = 5;
};

struct AecsClusterInfo {
    int index = -1;
    int performance_rank = -1;
    uint32_t min_freq = 0;
    uint32_t max_freq = 0;
    int capacity = 0;
    std::string tier;
    std::vector<int> cpu_ids;
};

struct AecsCpuTopology {
    std::vector<AecsClusterInfo> clusters_desc;
    std::vector<int> all_cpu_ids_desc;
    std::vector<int> prefill_order;
    std::vector<int> decode_stage1_order;
    std::unordered_map<int, int> cpu_to_cluster;
    std::string device_fingerprint;
    int biggest_capacity = 0;
    uint32_t biggest_freq = 0;
};

struct ThermalSample {
    bool thermal_valid = false;
    bool battery_valid = false;
    double thermal_c = 0.0;
    double battery_c = 0.0;
    bool overheating = false;
    std::string summary;
};

struct PowerSampleResult {
    bool valid = false;
    double energy_j = 0.0;
    double avg_power_w = 0.0;
    double duration_s = 0.0;
    int sample_count = 0;
};

struct AecsMeasurement {
    double speed_tok_s = 0.0;
    double time_s = 0.0;
    double energy_j = 0.0;
    double avg_power_w = 0.0;
    bool energy_valid = false;
};

struct AecsCandidateResult {
    std::vector<int> cpu_ids;
    int threads = 1;
    AecsMeasurement measurement;
    double heuristic_power = 0.0;
    double heuristic_energy = 0.0;
    double objective = 0.0;
    bool feasible = true;
    std::string source;
};

struct AecsStaticCalibrationResult {
    bool cache_hit = false;
    bool valid = false;
    int cluster_count = 0;
    std::vector<int> core_capacities;
    std::vector<double> cluster_ratios;
    std::vector<std::vector<int>> cluster_cpu_ids;
};

struct PhaseTuningResult {
    std::vector<int> prefill_cpu_ids;
    int prefill_threads = 0;
    std::vector<int> decode_cpu_ids;
    int decode_threads = 0;
    bool cache_hit = false;
    bool prefill_from_cache = false;
    bool decode_from_cache = false;
    AecsCandidateResult fastest_decode_candidate;
    AecsCandidateResult selected_decode_candidate;
    std::vector<AecsCandidateResult> decode_candidates;
    AecsStaticCalibrationResult static_calibration;
};

struct AecsCacheKey {
    std::string device_fingerprint;
    std::string model_path;
    std::string mnn_version;
    int backend = 0;
    int precision = 0;
    int memory = 0;
    int power = 0;
    int dynamic_option = 0;
    bool use_mmap = false;
    int n_prompt = 0;
};

class MNN_PUBLIC AecsCpuInspector {
public:
    static AecsCpuTopology inspect(int preferred_prefill_start_cpu = -1);
};

class MNN_PUBLIC ThermalGuard {
public:
    explicit ThermalGuard(const AecsTuningConfig& config);
    ~ThermalGuard();

    ThermalGuard(const ThermalGuard&) = delete;
    ThermalGuard& operator=(const ThermalGuard&) = delete;

    ThermalSample latestSample() const;
    void waitUntilCool(const std::string& reason) const;
    void checkAndPause(const std::string& reason) const;

private:
    void sampleLoop();

private:
    AecsTuningConfig mConfig;
    mutable std::mutex mMutex;
    mutable std::condition_variable mCondition;
    ThermalSample mLatest;
    bool mStop = false;
    std::thread mThread;
};

class MNN_PUBLIC EnergyProfiler {
public:
    explicit EnergyProfiler(const AecsTuningConfig& config);
    ~EnergyProfiler();

    EnergyProfiler(const EnergyProfiler&) = delete;
    EnergyProfiler& operator=(const EnergyProfiler&) = delete;

    bool available() const;
    void begin();
    PowerSampleResult end();

private:
    enum class SourceType : int {
        NONE = 0,
        SYSFS_POWER = 1,
        SYSFS_CURRENT_VOLTAGE = 2,
        DUMPSYS_BATTERY = 3,
    };

    struct Snapshot {
        bool valid = false;
        double power_w = 0.0;
        double timestamp_s = 0.0;
    };

    struct BatterySnapshot {
        bool valid = false;
        bool voltage_valid = false;
        bool current_valid = false;
        bool charge_counter_valid = false;
        bool external_power_valid = false;
        bool external_power = false;
        double timestamp_s = 0.0;
        double voltage_v = 0.0;
        double current_a = 0.0;
        long long charge_counter_uah = 0;
    };

    void sampleLoop();

private:
    AecsTuningConfig mConfig;
    mutable std::mutex mMutex;
    std::condition_variable mCondition;
    SourceType mSourceType = SourceType::NONE;
    std::string mPowerPath;
    std::string mCurrentPath;
    std::string mVoltagePath;
    bool mStop = false;
    bool mMeasuring = false;
    bool mAvailable = false;
    double mAccumulatedEnergyJ = 0.0;
    int mSampleCount = 0;
    Snapshot mLastSnapshot;
    Snapshot mMeasureBeginSnapshot;
    BatterySnapshot mMeasureBeginBatterySnapshot;
    std::thread mThread;
};

using PrefillMeasureFn = std::function<AecsMeasurement(const std::vector<int>& cpu_ids, int threads)>;
using DecodeMeasureFn = std::function<AecsMeasurement(const std::vector<int>& prefill_cpu_ids,
                                                      int prefill_threads,
                                                      const std::vector<int>& decode_cpu_ids,
                                                      int decode_threads)>;
using StaticCalibrationMeasureFn = std::function<AecsMeasurement(const std::vector<int>& cpu_ids,
                                                                 int threads,
                                                                 const std::vector<int>& core_capacities)>;

class MNN_PUBLIC AecsTuner {
public:
    AecsTuner(const AecsCpuTopology& topology,
              const AecsTuningConfig& config,
              const AecsHeuristicParams& heuristic_params,
              const std::vector<int>& allowed_cpu_ids = {});

    PhaseTuningResult tune(const AecsCacheKey& cache_key,
                           const std::vector<int>& manual_prefill_cpu_ids,
                           const std::vector<int>& manual_decode_cpu_ids,
                           int fallback_prefill_threads,
                           int fallback_decode_threads,
                           const PrefillMeasureFn& prefill_measure,
                           const DecodeMeasureFn& decode_measure) const;
    AecsStaticCalibrationResult calibrateStaticCapacities(const AecsCacheKey& cache_key,
                                                          const StaticCalibrationMeasureFn& measure) const;

    double heuristicPower(const std::vector<int>& cpu_ids) const;
    std::vector<std::vector<int>> buildPrefillCandidates() const;
    std::vector<std::vector<int>> buildDecodeStage2Candidates(const std::vector<int>& root_cpu_ids) const;

private:
    bool loadCache(const AecsCacheKey& cache_key, PhaseTuningResult* result) const;
    void saveCache(const AecsCacheKey& cache_key, const PhaseTuningResult& result) const;
    AecsStaticCalibrationResult tuneStaticCalibration(const StaticCalibrationMeasureFn& measure) const;
    AecsCandidateResult tunePrefill(const PrefillMeasureFn& prefill_measure) const;
    AecsCandidateResult tuneDecodeStage1(const std::vector<int>& prefill_cpu_ids,
                                         int prefill_threads,
                                         const DecodeMeasureFn& decode_measure) const;
    AecsCandidateResult tuneDecodeStage2(const std::vector<int>& prefill_cpu_ids,
                                         int prefill_threads,
                                         const AecsCandidateResult& fastest,
                                         std::vector<AecsCandidateResult>* all_candidates,
                                         const DecodeMeasureFn& decode_measure) const;
    std::vector<int> normalizeCpuIds(const std::vector<int>& cpu_ids) const;
    bool matchesCacheEntry(const AecsCacheKey& cache_key,
                           const AecsTuningConfig& cached_config,
                           const AecsHeuristicParams& cached_heuristic,
                           const AecsCacheKey& cached_key) const;
    bool matchesStaticCalibrationLayout(const AecsStaticCalibrationResult& result) const;

private:
    AecsCpuTopology mTopology;
    AecsTuningConfig mConfig;
    AecsHeuristicParams mHeuristicParams;
    std::vector<int> mAllowedCpuIds;
};

} // namespace Transformer
} // namespace MNN

#endif // LLM_AECS_TUNER_HPP
