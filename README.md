# Dispatcher (Visual Studio 2022)

This is a ready-to-build Visual Studio project for the **Real-Time Job Dispatcher (C++17 + SQLite)**.

## Prerequisites
1) Visual Studio 2022 with "Desktop development with C++".
2) SQLite precompiled binaries for Windows (sqlite3.dll, sqlite3.lib, sqlite3.h). Download from sqlite.org.
3) Edit `SQLite.paths.props` and set:
   - `SQLiteIncludeDir` → folder containing `sqlite3.h`
   - `SQLiteLibDir`     → folder containing `sqlite3.lib`
   - `SQLiteBinDir`     → folder containing `sqlite3.dll`

> On build, the project links against `sqlite3.lib` and copies `sqlite3.dll` to your output folder automatically.

## Build
- Open `Dispatcher.sln` in Visual Studio
- Select `x64` + `Debug` (or `Release`)
- Build → Build Solution

## Run
From the Visual Studio output dir (e.g., `bin\x64\Debug\`):

```
Dispatcher.exe --jobs 20 --max-retries 3 --mean-ms 400 --stddev-ms 120 --db result.db
```

## Notes
- The program creates `result.db` (or your chosen DB) with tables `runs` and `jobs`.
- You can inspect the DB using any SQLite viewer.
