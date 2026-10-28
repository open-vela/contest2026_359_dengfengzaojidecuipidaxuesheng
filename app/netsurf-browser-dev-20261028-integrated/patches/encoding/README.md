# TextDecoder encoding indexes

The unmodified WHATWG Encoding indexes are copied from
https://encoding.spec.whatwg.org/index-gb18030.txt and
https://encoding.spec.whatwg.org/index-gb18030-ranges.txt (2024-09-18 index dates).
`tools/gen_textdecoder_tables.py` validates their fixed SHA-256 hashes and
generates the native lookup arrays. Builds do not download or update them.

The generated tables and decoder algorithms incorporate the WHATWG work into
source code under its BSD 3-Clause option. The original copyright and complete
license text are retained in `../WHATWG-Encoding-LICENSE.txt` and packaged at
`/share/licenses/WHATWG-Encoding.txt`. Index source bytes are not modified;
generation changes the representation into C arrays.
