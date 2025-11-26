# 🚀 Real-Time Job Dispatcher Simulator (C++17 · SQLite · Priority Scheduling)

A production-grade **real-time job dispatcher simulator** demonstrating system design, OS-level scheduling, reliability engineering and database-backed observability.  
Built using **C++17**, **priority queues**, **exponential backoff**, and **SQLite** persistence.

Perfect project to showcase **system design + software engineering** skills in interviews.

---

## 📝 About the Project

This project simulates a real-world **job dispatching engine** — the type used in:

- Workflow / orchestration engines  
- Distributed task queues  
- Message brokers  
- ETL schedulers  
- High-throughput backend services  

It models realistic job lifecycle behavior:

### ✔ Priority Scheduling  
Jobs with higher priority run first.  
Tie-breaking uses enqueue time → FIFO fairness.

### ✔ Intelligent Retry Mechanism  
Jobs automatically retry on failure with:

- Exponential backoff (100 → 200 → 400ms...)
- Priority boosting to avoid starvation
- Configurable retry limits

### ✔ Full Lifecycle Metrics  
Each job tracks:

- Wait time  
- Service time  
- Turnaround time  
- Attempts  
- Final status (SUCCESS / FAILED)

### ✔ SQLite Persistence  
Every run and job is stored using prepared statements:

- `runs` table: high-level metrics  
- `jobs` table: per-job performance  

Allows easy data analysis and reporting.

### ✔ Failure Simulation  
Jobs fail probabilistically and often recover after retries — mimicking real distributed systems.

### ✔ Cross-Platform  
Works on:

- Windows (MSYS2/MinGW64, VS Code, or Visual Studio)
- Linux (GCC/clang)
- macOS (experimental)

---

## 📂 Project Structure

Real-Time-Job-Dispatcher/
├── src/
│ └── dispatcher.cpp # Main dispatcher implementation
├── docs/
│ └── ARCHITECTURE.md # Architecture & design explanation
├── sql/
│ └── analysis.sql # SQL for performance analysis
├── bin/ # Build output (ignored in Git)
├── .vscode/
│ ├── tasks.json # Build task (MSYS2 MinGW)
│ └── launch.json # Debug config
├── .gitignore
├── LICENSE
└── README.md


---

## 🛠️ Build & Run Instructions

# Windows (MSYS2 MinGW64)

### 1. Install build tools
```bash
pacman -Syu
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-gdb

g++ -std=c++17 -O2 src/dispatcher.cpp \
    -IC:/msys64/mingw64/include \
    -LC:/msys64/mingw64/lib \
    -lsqlite3 \
    -static-libgcc -static-libstdc++ \
    -o bin/dispatcher.exe

cp /c/msys64/mingw64/bin/libsqlite3-0.dll bin/

#### Run

./bin/dispatcher.exe --jobs 20 --max-retries 3 --mean-ms 400 --stddev-ms 120 --db result.db

## Linux
Build

sudo apt update
sudo apt install g++ libsqlite3-dev -y

g++ -std=c++17 -O2 src/dispatcher.cpp -lsqlite3 -o dispatcher

## Run
./dispatcher --jobs 20 --db dispatcher.db


#### Example Output
Dispatcher starting with 20 jobs...
[Job 4 | prio=10 | att=0] wait=12ms, service=311ms → SUCCESS
[Job 19 | prio=10 | att=0] ... → FAILED
[Job 19 | prio=10 | att=1] ... → SUCCESS
...

=== RUN SUMMARY ===
Total jobs: 20
Success:    20
Failed:     0
Avg Wait:   4380.35 ms
Avg Service:398.15 ms
Avg Turn:   4786.35 ms
Throughput: 1.48 jobs/s





