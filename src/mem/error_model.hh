#ifndef __MEM_ERROR_MODEL_HH__
#define __MEM_ERROR_MODEL_HH__

#include <vector>
#include <unordered_set>
#include <random>
#include <cstdint>
#include <algorithm>

#include "sim/stats.hh"
#include "sim/sim_object.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/Cache.hh"
#include "params/ErrorModel.hh" 

namespace gem5 {

class DRAMFailureModel : public SimObject
{
  public:
    /** Adjustable parameters for the DRAM failure model */
    struct Params : public SimObjectParams{
        double lambda_dev_per_hour = 66.1 / 1e9;
        double lambda_mbit_per_hour = 0.066 / 1e9;
        uint64_t device_mbit = 0;

        Tick epoch_ticks = 1000000;
        Tick soft_lifetime_ticks = 2000000;

        uint32_t lines_per_row = 1024;
        uint32_t rows_per_bank = 16384;
        uint32_t banks_per_rank = 8;
        uint32_t ranks = 2;

        uint32_t line_bytes = 64;

        uint64_t seed = 1;
        bool use_per_device = true;

        uint32_t max_lines_per_failure = 20000;
    };
    explicit DRAMFailureModel(const Params &p);

    void regStats() override;
    void tick(Tick now);

    bool activeAt(Addr lineAddr, Tick now, bool& out_is_hard);
    bool is_error(Addr lineAddr, Tick now, bool& out_is_hard);

    size_t numFailures() const;

    void printStats() const;

  private:
    /** statistics */
    statistics::Scalar total_failures;
    statistics::Scalar soft_failures;
    statistics::Scalar hard_failures;

    statistics::Scalar single_bit_errors;
    statistics::Scalar single_word_errors;
    statistics::Scalar single_column_errors;
    statistics::Scalar single_row_errors;
    statistics::Scalar single_bank_errors;
    statistics::Scalar multiple_bank_errors;
    statistics::Scalar multiple_rank_errors;

    struct Failure {
        enum Mode {
            SingleBit, SingleWord, SingleColumn, SingleRow,
            SingleBank, MultipleBank, MultipleRank
        } mode;

        bool is_hard = true;
        Tick birth_tick = 0;
        Tick expire_tick = 0;

        std::unordered_set<Addr> affected_lines;
    };

    Params P;
    std::mt19937_64 rng;

    std::vector<Failure> failures_;
    std::vector<size_t> active_ids_;

    Tick last_tick;

    std::discrete_distribution<int> modeDist_;
    double softProb_[7];
    std::uniform_real_distribution<double> U01{0.0, 1.0};

    static inline Addr alignLine_(Addr a, uint32_t line_bytes = 64);
    inline double ticksPerHour_() const;

    void initDistributions_();
    void spawnOneFailure_(Tick now);

    Addr randomLineAddr_();
    void addRowSpan_(Failure& f, Addr row_center, double row_percent);
    Addr rowBaseOf_(Addr line) const;
    void addRowSpread_(Failure& f, Addr row_any, double row_percent, double col_percent, double density=0.2);
    void addAffectedLines_(Failure& f, Addr seed);

    void gcExpired_(Tick now);
};

} // namespace gem5

#endif // __MEM_ERROR_MODEL_HH__
