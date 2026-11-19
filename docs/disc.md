# NTSC-U SLUS-20365 v1.00

Facts below come from a disc the project owner already has. They describe the image this scaffold is aimed at. The disc itself is not in the repository.

| Field | Value |
| --- | --- |
| Title | Pirates: The Legend of Black Kat |
| Publisher / developer | Electronic Arts / Westwood Studios, 2002 |
| Region | NTSC-U |
| Serial | SLUS-20365 |
| Version | 1.00 |
| Boot ELF | `SLUS_203.65` |
| `SYSTEM.CNF` | `BOOT2 = cdrom0:\SLUS_203.65;1` |
| Video mode | `VMODE` NTSC |

## ELF `SLUS_203.65`

| Field | Value |
| --- | --- |
| Size | 6,439,444 bytes |
| MD5 | `e6ea18b09f214dd276078872926ca10d` |
| Entry (`e_entry`) | `0x002A7B70` |
| Load address | `0x00100000` |
| Symbols | Stripped. No `.symtab`. |
| RTTI class names | about 311 |
| `Class::Method` strings | about 285 |
| Source filenames | 55 |
| `.text` | about 2.1 MB |
| `.vutext` | `0x3FC0` bytes of VU microcode |

`tools/extract_iso.sh` refuses any ELF whose size or MD5 differs. The game override in `src/overrides/blackkat_slus_20365.cpp` matches basename `SLUS_203.65` and entry `0x002A7B70`. Its CRC32 field is 0 until someone fills in the zlib CRC of this exact file (0 means the runtime does not check).

The PS2Recomp title list shipped in the pin contains `SLPS-20365` ("Saikyou Ginsei Shougi 4"), which is a different product. Do not treat that row as Black Kat.

## Other files on the disc

IOP replacement images:

- `IOPRP*.IMG`

IRX modules present on the disc:

- `LIBSD`
- `MCMAN`
- `MCSERV`
- `MTAPMAN`
- `PADMAN`
- `SDRDRV`
- `SIO2MAN`
- `SNDDRV`
- `USBD`

The extractor copies every `*.IRX` and `*.IMG`, not only this list, and it keeps paths relative to the ISO root.

Archives, not extracted by default:

| File | Note |
| --- | --- |
| `FILES.VIV` | EA BIGF-style archive. Layout not verified yet. |
| `NTSCVID.VIV` | Video archive, same unverified BIGF-style read. |
| `VID2.VIV` | Video archive. |
| `VID3.VIV` | Video archive. |

Those files are large. Pull a single one later with `7z` if a milestone needs it, for example `7z x -o/tmp/viv your.iso NTSCVID.VIV`. Do not commit the result.
