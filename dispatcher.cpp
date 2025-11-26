#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

// --------------------------- Utilities ---------------------------
struct Args {
    int jobs = 12;
    int max_retries = 2;
    int mean_ms = 300;
    int stddev_ms = 100;
    std::string db = "dispatcher.db";
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto need = [&](int &out){ if (i+1 < argc){ out = std::stoi(argv[++i]); } };
        auto needStr = [&](std::string &out){ if (i+1 < argc){ out = argv[++i]; } };
        if (k == "--jobs") need(a.jobs);
        else if (k == "--max-retries") need(a.max_retries);
        else if (k == "--mean-ms") need(a.mean_ms);
        else if (k == "--stddev-ms") need(a.stddev_ms);
        else if (k == "--db") needStr(a.db);
    }
    return a;
}

int64_t now_ms() {
    return std::chrono::duration_cast<Ms>(Clock::now().time_since_epoch()).count();
}

struct RNG {
    std::mt19937_64 gen{std::random_device{}()};
    std::normal_distribution<double> service_ms_norm;
    std::uniform_int_distribution<int> prio_dist{1, 10};
    std::bernoulli_distribution failure_dist{0.20}; // 20% base failure chance

    RNG(int mean_ms, int stddev_ms)
      : service_ms_norm(mean_ms, stddev_ms) {}

    int service_ms() {
        int v = (int)std::round(std::max(30.0, service_ms_norm(gen)));
        return v;
    }
    int priority() { return prio_dist(gen); }
    bool should_fail(int attempt) {
        // Slightly reduce failure chance with attempts to simulate fixes
        double p = std::max(0.02, 0.20 - 0.06 * attempt);
        std::bernoulli_distribution d(p);
        return d(gen);
    }
};

// --------------------------- Domain ---------------------------
struct Job {
    int ext_id;             // external ID (1..N)
    int priority;           // higher = sooner
    int attempt = 0;        // current attempt
    int max_retries;        // cap
    int64_t enqueue_ts;     // when enqueued (ms)
    std::optional<int64_t> start_ts;
    std::optional<int64_t> end_ts;
    int wait_ms = 0;
    int service_ms = 0;
    int turnaround_ms = 0;
    std::string status = "PENDING";  // PENDING/RUNNING/SUCCESS/FAILED
    std::string fail_reason;
};

struct JobCmp {
    bool operator()(Job const& a, Job const& b) const {
        // Higher priority first; if tie, earlier enqueue first
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.enqueue_ts > b.enqueue_ts;
    }
};

// --------------------------- SQLite helpers ---------------------------
struct DB {
    sqlite3* db = nullptr;
    explicit DB(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            std::cerr << "Failed to open DB: " << sqlite3_errmsg(db) << "\n";
            std::exit(1);
        }
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        create_schema();
    }
    ~DB(){ if (db) sqlite3_close(db); }

    void exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string m = err ? err : "unknown";
            sqlite3_free(err);
            std::cerr << "SQL error: " << m << "\n";
            std::exit(1);
        }
    }

    void create_schema() {
        exec(R"SQL(
        CREATE TABLE IF NOT EXISTS runs(
          run_id INTEGER PRIMARY KEY AUTOINCREMENT,
          started_at INTEGER,
          finished_at INTEGER,
          total_jobs INTEGER,
          success_jobs INTEGER,
          failed_jobs INTEGER,
          avg_wait_ms REAL,
          avg_service_ms REAL,
          avg_turnaround_ms REAL,
          throughput_jobs_per_s REAL
        );
        CREATE TABLE IF NOT EXISTS jobs(
          job_id INTEGER PRIMARY KEY AUTOINCREMENT,
          run_id INTEGER,
          ext_id INTEGER,
          priority INTEGER,
          attempt INTEGER,
          status TEXT,
          fail_reason TEXT,
          enqueue_ts INTEGER,
          start_ts INTEGER,
          end_ts INTEGER,
          wait_ms INTEGER,
          service_ms INTEGER,
          turnaround_ms INTEGER
        );
        )SQL");
    }
};

struct RunRecorder {
    DB& db;
    sqlite3_stmt* ins_job = nullptr;
    sqlite3_stmt* ins_run = nullptr;
    int64_t run_start_ms = 0;
    int64_t run_end_ms = 0;
    int run_id = 0;

    explicit RunRecorder(DB& d) : db(d) {
        prepare();
    }
    ~RunRecorder() {
        if (ins_job) sqlite3_finalize(ins_job);
        if (ins_run) sqlite3_finalize(ins_run);
    }
    void prepare() {
        const char* job_sql =
          "INSERT INTO jobs(run_id,ext_id,priority,attempt,status,fail_reason,"
          "enqueue_ts,start_ts,end_ts,wait_ms,service_ms,turnaround_ms)"
          " VALUES(?,?,?,?,?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db.db, job_sql, -1, &ins_job, nullptr) != SQLITE_OK)
            die("prepare ins_job");

        const char* run_sql =
          "INSERT INTO runs(started_at,finished_at,total_jobs,success_jobs,failed_jobs,"
          "avg_wait_ms,avg_service_ms,avg_turnaround_ms,throughput_jobs_per_s)"
          " VALUES(?,?,?,?,?,?,?,?,?);";
        if (sqlite3_prepare_v2(db.db, run_sql, -1, &ins_run, nullptr) != SQLITE_OK)
            die("prepare ins_run");
    }
    [[noreturn]] void die(const char* where) {
        std::cerr << where << " : " << sqlite3_errmsg(db.db) << "\n";
        std::exit(1);
    }

    void begin() { run_start_ms = now_ms(); }
    void end()   { run_end_ms   = now_ms(); }

    void record_job(const Job& j) {
        auto bind_text = [&](int idx, const std::string& s){
            if (sqlite3_bind_text(ins_job, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT) != SQLITE_OK) die("bind_text");
        };
        auto bind_int = [&](int idx, int v){
            if (sqlite3_bind_int(ins_job, idx, v) != SQLITE_OK) die("bind_int");
        };
        auto bind_i64 = [&](int idx, int64_t v){
            if (sqlite3_bind_int64(ins_job, idx, (sqlite3_int64)v) != SQLITE_OK) die("bind_i64");
        };

        bind_int(1, run_id);
        bind_int(2, j.ext_id);
        bind_int(3, j.priority);
        bind_int(4, j.attempt);
        bind_text(5, j.status);
        bind_text(6, j.fail_reason);
        bind_i64(7, j.enqueue_ts);
        bind_i64(8, j.start_ts.value_or(0));
        bind_i64(9, j.end_ts.value_or(0));
        bind_int(10, j.wait_ms);
        bind_int(11, j.service_ms);
        bind_int(12, j.turnaround_ms);

        if (sqlite3_step(ins_job) != SQLITE_DONE) die("step ins_job");
        sqlite3_reset(ins_job);
        sqlite3_clear_bindings(ins_job);
    }

    void record_run_summary(int total, int succ, int fail,
                            double avg_wait, double avg_service, double avg_turn,
                            double throughput) {
        auto bind_i64 = [&](int idx, int64_t v){
            if (sqlite3_bind_int64(ins_run, idx, (sqlite3_int64)v) != SQLITE_OK) die("bind i64 run");
        };
        auto bind_int = [&](int idx, int v){
            if (sqlite3_bind_int(ins_run, idx, v) != SQLITE_OK) die("bind int run");
        };
        auto bind_d = [&](int idx, double v){
            if (sqlite3_bind_double(ins_run, idx, v) != SQLITE_OK) die("bind dbl run");
        };

        bind_i64(1, run_start_ms);
        bind_i64(2, run_end_ms);
        bind_int(3, total);
        bind_int(4, succ);
        bind_int(5, fail);
        bind_d(6, avg_wait);
        bind_d(7, avg_service);
        bind_d(8, avg_turn);
        bind_d(9, throughput);

        if (sqlite3_step(ins_run) != SQLITE_DONE) die("step ins_run");
        run_id = (int)sqlite3_last_insert_rowid(db.db);
        sqlite3_reset(ins_run);
        sqlite3_clear_bindings(ins_run);
    }
};

// --------------------------- Dispatcher ---------------------------
struct Dispatcher {
    Args args;
    RNG rng;
    DB db;
    RunRecorder rec;

    std::priority_queue<Job, std::vector<Job>, JobCmp> pq;
    std::vector<Job> completed;

    Dispatcher(const Args& a)
      : args(a), rng(a.mean_ms, a.stddev_ms), db(a.db), rec(db) {}

    void seed_jobs() {
        int64_t t0 = now_ms();
        for (int i = 1; i <= args.jobs; ++i) {
            Job j;
            j.ext_id = i;
            j.priority = rng.priority();
            j.max_retries = args.max_retries;
            j.enqueue_ts = t0 + i; // stable ordering
            pq.push(j);
        }
    }

    void maybe_backoff(Job& j) {
        if (j.attempt == 0) return;
        // exponential backoff: 100ms, 200ms, 400ms...
        int backoff = 100 << (j.attempt - 1);
        std::this_thread::sleep_for(Ms(backoff));
    }

    void run() {
        rec.begin();
        seed_jobs();
        auto wall_start = Clock::now();

        int successes = 0, failures = 0;
        int64_t total_wait = 0, total_service = 0, total_turn = 0;

        while (!pq.empty()) {
            Job j = pq.top(); pq.pop();

            maybe_backoff(j);

            j.start_ts = now_ms();
            j.status = "RUNNING";
            j.wait_ms = (int)(*j.start_ts - j.enqueue_ts);

            int svc = rng.service_ms();
            j.service_ms = svc;
            std::this_thread::sleep_for(Ms(svc));

            bool fail = rng.should_fail(j.attempt);
            j.end_ts = now_ms();
            j.turnaround_ms = (int)(*j.end_ts - j.enqueue_ts);

            if (!fail) {
                j.status = "SUCCESS";
                successes++;
                total_wait += j.wait_ms;
                total_service += j.service_ms;
                total_turn += j.turnaround_ms;
                rec.record_job(j);
                completed.push_back(j);
                log_console(j);
            } else {
                j.status = "FAILED";
                j.fail_reason = "SIMULATED_FAILURE";
                rec.record_job(j);
                log_console(j);

                if (j.attempt < j.max_retries) {
                    j.attempt += 1;
                    j.status = "PENDING";
                    // Re-enqueue with slight priority aging to avoid starvation
                    j.priority = std::min(10, j.priority + 1);
                    j.enqueue_ts = now_ms();
                    pq.push(j);
                } else {
                    failures++;
                    completed.push_back(j);
                }
            }
        }

        rec.end();
        double seconds = std::max(0.001,
            std::chrono::duration<double>(Clock::now() - wall_start).count());
        int total = successes + failures;

        double avg_wait = total ? (double)total_wait / total : 0.0;
        double avg_service = total ? (double)total_service / total : 0.0;
        double avg_turn = total ? (double)total_turn / total : 0.0;
        double throughput = seconds > 0 ? (double)successes / seconds : 0.0;

        rec.record_run_summary(total, successes, failures,
                               avg_wait, avg_service, avg_turn, throughput);

        std::cout << "\n=== RUN SUMMARY ===\n"
                  << "Total jobs: " << total << "\n"
                  << "Success:    " << successes << "\n"
                  << "Failed:     " << failures << "\n"
                  << "Avg Wait:   " << std::fixed << std::setprecision(2) << avg_wait << " ms\n"
                  << "Avg Service:" << avg_service << " ms\n"
                  << "Avg Turn:   " << avg_turn << " ms\n"
                  << "Throughput: " << throughput << " jobs/s\n";
    }

    void log_console(const Job& j) {
        std::cout << "[Job " << j.ext_id
                  << " | prio=" << j.priority
                  << " | att=" << j.attempt
                  << "] wait=" << j.wait_ms
                  << "ms, service=" << j.service_ms
                  << "ms, turn=" << j.turnaround_ms
                  << "ms -> " << j.status;
        if (!j.fail_reason.empty()) std::cout << " (" << j.fail_reason << ")";
        std::cout << "\n";
    }
};

// --------------------------- main ---------------------------
int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    std::cout << "Dispatcher starting with "
              << args.jobs << " jobs, max_retries=" << args.max_retries
              << ", mean=" << args.mean_ms << "ms, stddev=" << args.stddev_ms
              << "ms, db=" << args.db << "\n";

    Dispatcher d(args);
    d.run();
    return 0;
}
