Vendored unchanged from Seeed_GFX2 commit
`1a415dd092bf33c0e6d9ed04a9da6bc6edf047f8`, example
`examples/ePaper Displays/reTerminal E Series/reTerminal Sticky/reTerminal_Sticky_SDcard_BW/miniz.c`.

This is miniz 1.15 with Seeed's decompression-only configuration. Its license
is included at the end of miniz.c. The dashboard uses only CRC32 and the bounded
low-level tinfl API; the same source is built on the host and ESP32.
