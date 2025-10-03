#include "mem/error_model.hh"
#include "debug/Cache.hh"
#include "base/random.hh"
#include <iostream>

namespace gem5 {

DRAMFailureModel::DRAMFailureModel(const Params &p)
    : SimObject(p), P(p), rng(p.seed)  
{
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
    last_tick = 0;
    initDistributions_();
}

void
DRAMFailureModel::regStats()
{
    using namespace statistics;

    total_failures
        .name(name() + ".total_failures")
        .desc("Total DRAM failures observed");

    soft_failures
        .name(name() + ".soft_failures")
        .desc("Soft (transient) failures");

    hard_failures
        .name(name() + ".hard_failures")
        .desc("Hard (permanent) failures");

    single_bit_errors
        .name(name() + ".single_bit_errors")
        .desc("Single-bit errors");

    single_word_errors
        .name(name() + ".single_word_errors")
        .desc("Single-word errors");

    single_column_errors
        .name(name() + ".single_column_errors")
        .desc("Single-column errors");

    single_row_errors
        .name(name() + ".single_row_errors")
        .desc("Single-row errors");

    single_bank_errors
        .name(name() + ".single_bank_errors")
        .desc("Single-bank errors");

    multiple_bank_errors
        .name(name() + ".multiple_bank_errors")
        .desc("Multiple-bank errors");

    multiple_rank_errors
        .name(name() + ".multiple_rank_errors")
        .desc("Multiple-rank errors");
}

void
DRAMFailureModel::tick(Tick now)
{
    if (P.epoch_ticks == 0) return;
    if (last_tick == 0) last_tick = now;

    Tick dt = now - last_tick;
    if (dt < P.epoch_ticks) return;

    DPRINTF(Cache, "[Error model] tick @ %lu, dt=%lu\n", now, dt);
    double delta_hours = double(dt) / ticksPerHour_();
    double lambda = P.use_per_device
                  ? P.lambda_dev_per_hour
                  : (P.lambda_mbit_per_hour * double(P.device_mbit));

    double p_new =1;//std::min(0.999, std::max(0.0, lambda * delta_hours));

    std::bernoulli_distribution bern(p_new);
    while (bern(rng)) {
        DPRINTF(Cache, "[Error model] Spawning new failure @ tick %lu\n", now);
        spawnOneFailure_(now);
    }

    gcExpired_(now);
    last_tick = now;
}

bool
DRAMFailureModel::activeAt(Addr lineAddr, Tick now, bool& out_is_hard)
{
    return is_error(lineAddr, now, out_is_hard);
}

bool
DRAMFailureModel::is_error(Addr lineAddr, Tick now, bool& out_is_hard)
{
    lineAddr = alignLine_(lineAddr);

    for (auto id : active_ids_) {
        auto &f = failures_[id];
        if (f.expire_tick && now >= f.expire_tick) continue;
        if (f.affected_lines.count(lineAddr)) {
            out_is_hard = f.is_hard;
            return true;
        }
    }
    out_is_hard = false;
    return false;
}

size_t
DRAMFailureModel::numFailures() const
{
    return active_ids_.size();
}

void
DRAMFailureModel::printStats() const
{
    std::cout << "Error Model Statistics:" << std::endl;
    std::cout << "Total Failures: " << total_failures.value() << std::endl;
    std::cout << "Soft Failures: " << soft_failures.value() << std::endl;
    std::cout << "Hard Failures: " << hard_failures.value() << std::endl;
    std::cout << "Single Bit Errors: " << single_bit_errors.value() << std::endl;
    std::cout << "Single Word Errors: " << single_word_errors.value() << std::endl;
    std::cout << "Single Column Errors: " << single_column_errors.value() << std::endl;
    std::cout << "Single Row Errors: " << single_row_errors.value() << std::endl;
    std::cout << "Single Bank Errors: " << single_bank_errors.value() << std::endl;
    std::cout << "Multiple Bank Errors: " << multiple_bank_errors.value() << std::endl;
    std::cout << "Multiple Rank Errors: " << multiple_rank_errors.value() << std::endl;
}

/** private helpers **/

inline Addr
DRAMFailureModel::alignLine_(Addr a, uint32_t line_bytes)
{
    return (a & ~Addr(line_bytes - 1));
}

inline double
DRAMFailureModel::ticksPerHour_() const
{
    return 3600.0 * 1e12; // 1 tick = 1 ps
}

void
DRAMFailureModel::initDistributions_()
{
    std::vector<double> w = {49.7, 2.5, 10.6, 12.7, 16.3, 2.5, 5.5};
    modeDist_ = std::discrete_distribution<int>(w.begin(), w.end());

    auto S = [&](Failure::Mode m, double soft_percent) {
        softProb_[int(m)] = soft_percent / 100.0;
    };
    S(Failure::SingleBit,    43.3);
    S(Failure::SingleWord,   81.4);
    S(Failure::SingleColumn, 19.7);
    S(Failure::SingleRow,     2.8);
    S(Failure::SingleBank,    7.0);
    S(Failure::MultipleBank, 17.5);
    S(Failure::MultipleRank, 24.3);
}

void
DRAMFailureModel::spawnOneFailure_(Tick now)
{
    Failure f{};
    f.mode = Failure::Mode(modeDist_(rng));
    {
        double psoft = softProb_[int(f.mode)];
        f.is_hard = (U01(rng) >= psoft);
        if (!f.is_hard) {
            f.expire_tick = now + (P.soft_lifetime_ticks ? P.soft_lifetime_ticks : (P.epoch_ticks));
        }
    }
    f.birth_tick = now;

    
    Addr seed_line = randomLineAddr_();
    
    addAffectedLines_(f, seed_line);

    total_failures++;
    if (f.is_hard) {
        hard_failures++;
    } else {
        soft_failures++;
    }
    switch (f.mode) {
        case Failure::SingleBit:
            single_bit_errors++;
            break;
        case Failure::SingleWord:
            single_word_errors++;
            break;
        case Failure::SingleColumn:
            single_column_errors++;
            break;
        case Failure::SingleRow:
            single_row_errors++;
            break;
        case Failure::SingleBank:
            single_bank_errors++;
            break;
        case Failure::MultipleBank:
            multiple_bank_errors++;
            break;
        case Failure::MultipleRank:
            multiple_rank_errors++;
            break;
    }

    if (P.max_lines_per_failure && f.affected_lines.size() > P.max_lines_per_failure) {
        
        size_t keep = P.max_lines_per_failure;
        std::vector<Addr> tmp;
        tmp.reserve(f.affected_lines.size());
        for (auto a : f.affected_lines) tmp.push_back(a);
        std::shuffle(tmp.begin(), tmp.end(), rng);
        f.affected_lines.clear();
        for (size_t i = 0; i < keep; ++i) f.affected_lines.insert(tmp[i]);
    }

    
    size_t id = failures_.size();
    failures_.push_back(std::move(f));
    active_ids_.push_back(id);
}
Addr 
DRAMFailureModel::randomLineAddr_()
{
    
    uint64_t raw = rng() & ((1ULL<<48)-1);
    return alignLine_(raw, P.line_bytes);
}

void 
DRAMFailureModel::addRowSpan_(Failure& f, Addr row_center, double row_percent) {
    uint32_t width = std::max(1u, uint32_t( (row_percent/100.0) * P.lines_per_row ));
    int32_t half = int32_t(width/2);
    Addr base = rowBaseOf_(row_center);
    int32_t center_idx = int32_t( (row_center - base) / P.line_bytes );
    int32_t lo = std::max(0, center_idx - half);
    int32_t hi = std::min<int32_t>(P.lines_per_row-1, center_idx + half);

    for (int32_t i = lo; i <= hi; ++i) {
        f.affected_lines.insert(base + Addr(i) * P.line_bytes);
    }
}
Addr 
DRAMFailureModel::rowBaseOf_(Addr line) const {
    Addr line_idx = (line / P.line_bytes);
    Addr row_idx = (line_idx / P.lines_per_row);
    Addr row_base = row_idx * P.lines_per_row * P.line_bytes;
    return row_base;
}
void
DRAMFailureModel::addRowSpread_(Failure& f, Addr row_any, double row_percent, double col_percent, double density) {
        uint32_t rows = std::max(1u, uint32_t((row_percent/100.0) * P.rows_per_bank));
        uint32_t per_row = std::max(1u, uint32_t( (col_percent/100.0) * P.lines_per_row * density ));
        for (uint32_t r=0; r<rows; ++r) {
            Addr row_base = rowBaseOf_(row_any) + Addr(r) * P.lines_per_row * P.line_bytes;
            for (uint32_t k=0; k<per_row; ++k) {
                uint32_t col = uint32_t(rng() % P.lines_per_row);
                f.affected_lines.insert(row_base + Addr(col) * P.line_bytes);
            }
        }
    }
void 
DRAMFailureModel::addAffectedLines_(Failure& f, Addr seed) {
    auto U = [&](double a, double b){ std::uniform_real_distribution<double> d(a,b); return d(rng); };
    auto drawMixture = [&](double p){ return U01(rng) < p; };

    switch (f.mode) {
    case Failure::SingleBit:
        f.affected_lines.insert(seed);
        break;

    case Failure::SingleWord:
        f.affected_lines.insert(seed);
        for (int i=0;i<int(U(1,4));++i) {
            Addr neigh = seed + Addr((rng()%3)-1) * P.line_bytes;
            f.affected_lines.insert(neigh);
        }
        break;

    case Failure::SingleColumn: {
        double rows_pct = U(0.5, 6.0);
        uint32_t rows = std::max(1u, uint32_t((rows_pct/100.0) * P.rows_per_bank));
        for (uint32_t r=0; r<rows; ++r) {
            addRowSpan_(f, seed + Addr(r)*P.lines_per_row*P.line_bytes, /*row_percent=*/0.5);
        }
        break;
    }

    case Failure::SingleRow: {
        double cols_pct = drawMixture(0.10) ? U(25.0,30.0) : U(1.0,10.0);
        addRowSpan_(f, seed, cols_pct);
        break;
    }

    case Failure::SingleBank: {
        // rows¡GMix[80%¡PU(1,5), 20%¡PU(5,10)]
        double rows_pct = drawMixture(0.20) ? U(5.0,10.0) : U(1.0,5.0);
        // cols¡GMix[80%¡PU(1,5), 20%¡PU(20,40)]
        double cols_pct = drawMixture(0.20) ? U(20.0,40.0) : U(1.0,5.0);
        bool row_cluster = drawMixture(0.50);

        if (row_cluster) {  
            uint32_t rows = std::max(1u, uint32_t((rows_pct/100.0) * P.rows_per_bank));
            for (uint32_t r=0; r<rows; ++r)
                addRowSpan_(f, seed + Addr(r)*P.lines_per_row*P.line_bytes, cols_pct);
        } else {
            addRowSpread_(f, seed, rows_pct, cols_pct, /*density=*/0.2);
        }
        break;
    }

    case Failure::MultipleBank: {
        int choices[6]   = {2,3,4,5,6,8};
        double prob[6]   = {46.2,1.9,3.8,7.7,3.8,36.5};
        std::discrete_distribution<int> d(std::begin(prob), std::end(prob));
        int nbank = choices[d(rng)];
        for (int b=0; b<nbank; ++b) {
            Failure dummy; dummy.mode = Failure::SingleBank;
            dummy.is_hard = f.is_hard; dummy.birth_tick = f.birth_tick;
            addAffectedLines_(dummy, seed + Addr(b)*P.lines_per_row*P.line_bytes*100);
            f.affected_lines.insert(dummy.affected_lines.begin(), dummy.affected_lines.end());
        }
        break;
    }

    case Failure::MultipleRank: {
        int choices[7] = {2,3,4,5,6,7,8};
        double prob[7] = {4.0,2.0,3.0,14.1,7.1,15.2,54.5};
        std::discrete_distribution<int> d(std::begin(prob), std::end(prob));
        int nbank = choices[d(rng)];

        for (int b=0; b<nbank; ++b) {
            double rows_pct =  U(1.0,5.0);
            double cols_pct =  U(1.0,5.0);
            bool row_cluster = (U01(rng) < 0.2);
            if (row_cluster) {
                addRowSpan_(f, seed + Addr(b)*P.lines_per_row*P.line_bytes*200, std::min(40.0, cols_pct+20.0));
            } else {
                addRowSpread_(f, seed + Addr(b)*P.lines_per_row*P.line_bytes*200, rows_pct, cols_pct, /*density=*/0.25);
            }
        }
        break;
    }
    }
}

void DRAMFailureModel::gcExpired_(Tick now)
{
    auto it = active_ids_.begin();
    while (it != active_ids_.end()) {
        size_t id = *it;
        auto &f = failures_[id];
        if (f.expire_tick && now >= f.expire_tick) {
            it = active_ids_.erase(it);
        } else {
            ++it;
        }
    }
}


} // namespace gem5
