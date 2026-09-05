Parsing and Serialization Utilities
===================================

``include/Utils/Formats/`` and ``lib/Utils/Formats/`` provide lightweight data
format support for configuration, interchange, and debugging output.

**Main components**:

- ``SExpr`` for S-expression parsing.
- ``cJSON`` and ``json11`` for JSON handling.
- ``toml`` for TOML configuration file parsing.
- ``pcomb/`` for parser-combinator based parsers.

These helpers show up throughout Lotus in spec loaders, report generation, and
small domain-specific parsers.

Choosing a format helper
------------------------

Use the existing parser closest to the on-disk format instead of adding a new
ad hoc decoder.  Spec readers should validate required fields and report the
source location or key that failed to parse.  JSON and TOML are suited to
structured configuration and interchange, while ``SExpr`` and ``pcomb`` are
useful for compact, grammar-oriented internal languages.

See also :doc:`utilities`.
