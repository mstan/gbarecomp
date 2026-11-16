# Optional ROM archive input

Desktop titles can enable `GBARECOMP_ROM_ARCHIVES` at build time and set
`RunOptions::enable_rom_archives`. The optional dependency is libarchive 3.8.11
or newer. Builds without it keep raw ROM input and never advertise archives.

The launcher and runtime use the same decoder for ZIP, RAR (including RAR5) and
7z. Only a regular member with the title's exact expected SHA-1 is selected.
Unrelated files are ignored; byte-identical matching copies are one choice.
Raw ROMs still take priority when their identity matches, even if their filename
or leading bytes resemble an archive.

Archives must be unencrypted and single-part. Unsupported compression methods,
damaged archives and missing compatible images report an error. Input is bounded
to 128 MiB compressed, 32 MiB per member, 256 MiB total decoded data and 1024
entries. No member is extracted to the filesystem. Links cannot supply a ROM.

The original selected path is retained in the existing ROM cache and passed to
the existing native-save path policy. No writable-root selection, config name,
explicit save override or title save namespace is changed by decoding.

The UI byte-reader callback is optional. Its decoded bytes still pass the UI's
normal fingerprint checks; the callback does not grant a verification verdict.
Hosts providing it must use identical decoding when launching the selected path.
