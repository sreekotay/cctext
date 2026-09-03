# Unicode grapheme-break tables

`utf8_break_tab.cch` is generated from Unicode **17.0.0** UAX #29 data.
Smokes use the checked-in table and `testdata/unicode/GraphemeBreakTest.txt`
(no network).

## Regenerate

From the repo root (downloads UCD into `testdata/unicode/` if missing):

```bash
python3 scripts/gen_unicode_gcb.py
```

Pinned URLs (`UNICODE_VERSION` in the script):

- https://www.unicode.org/Public/17.0.0/ucd/auxiliary/GraphemeBreakProperty.txt
- https://www.unicode.org/Public/17.0.0/ucd/emoji/emoji-data.txt
- https://www.unicode.org/Public/17.0.0/ucd/DerivedCoreProperties.txt (InCB / GB9c)
- https://www.unicode.org/Public/17.0.0/ucd/auxiliary/GraphemeBreakTest.txt

`--check` rebuilds the in-memory table and walks GraphemeBreakTest without
writing the header. Only `GraphemeBreakTest.txt` is meant to be committed
under `testdata/unicode/`; the other UCD dumps are regen inputs.
