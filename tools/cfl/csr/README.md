# Context-Sensitive Reachability (CSR) Tool

The tool uses the FLARE APIs under ``include/CFL/CSIndex/FLARE``. The separate
sanitizer-aware extension lives under ``include/CFL/CSIndex/SCS`` and is not
selected by this command-line driver.

The CSR tool provides indexing-based context-sensitive reachability analysis for large-scale program analysis.

## Features

- **Multiple Indexing Algorithms**:
  - GRAIL (Graph Random Labeling)
  - PathTree (Path Tree Indexing)
  - Combined PathTree+GRAIL

- **Query Types**:
  - Reachability queries
  - Transitive closure computation

- **Parallel Processing**:
  - Parallel Tabulation algorithm for improved performance on large graphs

## Usage

```bash
./csr [options] graph_file
```

## Options

| Option | Description |
|--------|-------------|
| `-h` | Print help message |
| `-n N` | Number of reachable/unreachable queries to generate (default: 100 each) |
| `-g file` | Save generated queries to file |
| `-q file` | Read queries from file |
| `-t` | Evaluate transitive closure |
| `-r` | Evaluate Rep's tabulation algorithm |
| `-p` | Evaluate parallel tabulation algorithm |
| `-j N` | Number of threads for parallel tabulation (0 = auto-detect) |
| `-m method` | Indexing approach: pathtree, grail, or pathtree+grail |
| `-d N` | Dimension for GRAIL labeling (default: 2) |

## Examples

```bash
# Basic usage with GRAIL indexing
./csr input.graph

# Use PathTree indexing
./csr -m pathtree input.graph

# Evaluate parallel tabulation with 4 threads
./csr -p -j 4 input.graph

# Generate and save queries for later use
./csr -g queries.txt -n 1000 input.graph

# Load pre-generated queries
./csr -q queries.txt input.graph

# Compute transitive closure
./csr -t input.graph
```

## Parallel Tabulation

The parallel tabulation algorithm (`-p`) provides improved performance on large graphs by:

- **Automatic thread detection**: Uses `std::thread::hardware_concurrency()` when `-j 0`
- **Thread-local visited sets**: Avoids contention between threads
- **Configurable parallelism**: Specify exact number of threads with `-j N`
- **Same interface**: Drop-in replacement for the original tabulation algorithm

## Output Format

The tool outputs comma-separated values (CSV) to stderr for easy parsing:

```
edges,vertices,summary_edge_time,pt_total_duration,grail_on_ig_duration,,summary_edge_size,pt_total_size,grail_on_ig_size,,tc_time,tc_size,pt_r_time,pt_nr_time,,grail_r_time,grail_nr_time,,tab_r_query_time,tab_notr_query_time,,parallel_tc_time,parallel_tc_size,
```

## Performance

- **Memory efficient**: Uses compressed indexing structures
- **Scalable**: Handles graphs with millions of vertices
- **Fast queries**: Sub-millisecond query times for most cases
- **Parallel speedup**: Significant improvement on multi-core systems
