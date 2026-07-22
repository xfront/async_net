# async_net — TechEmpower Framework Benchmarks

This directory contains the [TechEmpower Framework Benchmarks](https://www.techempower.com/benchmarks/) implementation for async_net.

## Test Types

All 7 standard TechEmpower test types are implemented:

| # | Test | Endpoint | Description |
|---|------|----------|-------------|
| 1 | JSON Serialization | `GET /json` | Returns `{"message":"Hello, World!"}` |
| 2 | Single DB Query | `GET /db` | Returns a random World row as JSON |
| 3 | Multiple DB Queries | `GET /queries?queries=N` | Returns N random World rows (1-500) |
| 4 | Fortunes | `GET /fortunes` | Renders sorted fortunes as HTML with XSS escaping |
| 5 | DB Updates | `GET /updates?queries=N` | Fetches, updates, and returns N World rows |
| 6 | Plaintext | `GET /plaintext` | Returns `Hello, World!` as plain text |
| 7 | Caching | `GET /cached-queries?count=N` | Returns N CachedWorld rows from memory |

## Build

### As part of the main project

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target te_bench -j$(nproc)
```

### Standalone

```bash
cd benchmarks/async_net
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

```bash
./build/benchmarks/async_net/te_bench [port]
# Default port: 8080
```

## Implementation Notes

- **Database**: World and Fortune tables are simulated in-memory (10,000 World rows, 12 Fortune rows). For TechEmpower classification, this is "Raw" ORM with no external database.
- **HTTP**: Uses async_net's HTTP/1.1 server with keep-alive support.
- **Serialization**: Minimal hand-written JSON serialization (no external JSON library).
- **XSS Protection**: Fortune messages are HTML-escaped (`<`, `>`, `&`, `"`, `'`).
- **UTF-8**: Japanese fortune cookie message is preserved correctly.
- **Caching**: CachedWorld uses the same in-memory table (no separate cache process needed).

## TechEmpower Configuration

See `benchmark_config.json` for the framework metadata.

## Docker

```bash
docker build -t async_net_bench .
docker run -p 8080:8080 async_net_bench
```
