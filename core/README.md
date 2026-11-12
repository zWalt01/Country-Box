# core/

The recompilation core: the ARM64 → C lifter and code generator (`recomp/`), the host runtime that replaces the
Switch OS and renders NVN through Vulkan (`runtime/`), the shader translator and tools (`tools/`), and tests
(`tests/`).

It comes from [Metroid Prime 4 Recomp](https://github.com/jaigner-hub/metroid-prime-4-recomp) by Jeff Aigner (MIT)
at commit `6c9c590`, which carries the lineage of fe3h-pc-recomp, totk-pc-recomp, LAS Recomp and others. Every
copied or adapted part is listed with its source, commit and license in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Country-Box ships no extraction or decryption tools: the upstream installer is not part of this tree.
