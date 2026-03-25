# SymAbsAI Configuration Guide

Everything you need to run SymAbsAI lives in this directory. Pick a config, point `spranalyze` or `symabs_ai` at it, and go.

## TL;DR
- `symabs_ai --list-configs` (or `spranalyze --list-configs`) shows the menu.
- `symabs_ai --config config/symabs-ai/FILE.conf input.bc` is the common path.
- `--abstract-domain NAME` and `--function main` override config values.
- Set `SYMBOLIC_ABSTRACTION_CONFIG=config/symabs-ai/FILE.conf` to make a default.

## Quick Commands

```bash
symabs_ai --list-domains
symabs_ai --list-configs
symabs_ai --config config/symabs-ai/03_const_edge.conf program.bc
symabs_ai --abstract-domain SimpleConstProp --function main program.bc
symabs_ai --verbose --config config/symabs-ai/17_const_function_verbose.conf debug.bc
```

## Ready-Made Configs

| Goal | Config | Notes |
|------|--------|-------|
| Fast constant propagation | `01_const_function.conf` | Whole function, no loops |
| Loop-friendly constants | `03_const_edge.conf` | Balanced default |
| Relations & inequalities | `05_const_rel_edge.conf` | Const prop + NumRels |
| Memory safety (SV-COMP) | `memsafety_sv16_body.conf` | MemRegion + NumRels |
| Verbose debugging | `17_const_function_verbose.conf` | Spews transformers |
| Max precision | `all_serializable_domains.conf` | Slow, exhaustive |

## Config Format Snapshot

```ini
# Comments start with #
FragmentDecomposition.Strategy = Edges      # Function|Edges|Headers|Body|Backedges
AbstractDomain = SimpleConstProp, Interval  # Comma-separated for products
MemoryModel.Variant = BlockModel            # NoMemory|BlockModel|Aligned|LittleEndian
Analyzer.WideningDelay = 20
SymAbsAIPass.Verbose = false
```

**Overrides:** CLI flags always win. Combine configs with `--abstract-domain` etc. when you need to experiment.

## Pick a Workflow
- **First look at a module:** `spranalyze --list-functions module.bc`
- **Find constants quickly:** `spranalyze --abstract-domain SimpleConstProp --function main module.bc`
- **Check bounds:** `spranalyze --abstract-domain Interval --function compute module.bc`
- **Audit memory:** `symabs_ai --config config/symabs-ai/memsafety_sv16_body.conf unsafe.bc`
- **Tune your own:** copy a `.conf`, adjust strategy, domains, widening delay.

## Migration Snapshot
- All 20 `.conf` files live here; they replace the old Python configs.
- Custom parser in `lib/Verification/SymAbsAI/Utils/Config.cpp` loads plain `key=value` files and respects `SYMBOLIC_ABSTRACTION_CONFIG`.
- CLI adds `--config` and `--list-configs`, richer `--help`, domain summaries, and verbose banners.
- Documentation merged: this README now covers quick reference + migration status.

**Sanity checks after changes**
- Load a few configs: `symabs_ai --config config/symabs-ai/01_const_function.conf test.bc`
- Confirm environment variable path works.
- Run `spranalyze --list-configs` to ensure new files are picked up.

## Troubleshooting Cheatsheet

| Symptom | Fix |
|---------|-----|
| Config not found | Use repository-relative path or absolute path; ensure file readable |
| Unknown domain | `spranalyze --list-domains`, double-check spelling/case |
| Function rejected | Run `opt -mem2reg < in.bc > out.bc` to ensure SSA |
| No output | Add `--verbose` or use any `*_verbose.conf` |
| Config ignored | Remember CLI flags override file entries |

## File Map & Further Reading
- `config/symabs-ai/*.conf` – ready-to-use profiles (see headers in each file).
- `tools/verifier/symabs-ai/spranalyze.cpp` – CLI entry point with option docs.
- `docs/source/verification/symabs-ai.rst` – framework documentation.

Questions? Start with `spranalyze --help`, then skim the configs themselves for inline comments.

