# EXT4 migration — 2026-10-30

**Source import, not yet a working independent EXT4.so.**

EXT4 canonical core and design copied unchanged from Filesystems commit ca2fa1210c3c1f2b6f52cd15b3da6dfa83d453ad. Shared EXT2/3/4 Defragmenter analysis/relocation sources copied unchanged to `migration/defragmenter/native/` from Defragmenter commit 30af892d8897d3aeaeefeac1bfcdb9620a0a5c72. This is one shared migration reference, not an EXT4-only implementation and NOT to be independently forked into EXT2 and EXT3.

Full Linux kernel implementation still lives in Filesystems and existing Defragmenter builds still own runtime dispatch. Do not remove either until an integrated standalone module passes independent read/write/defrag tests and Common ABI adoption. No new compiled driver or release is asserted.
