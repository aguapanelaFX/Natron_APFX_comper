# SOLVFX USD bridge

`solvusd` is a small C interface around OpenUSD (core, monolithic, no imaging / python), built with
Microsoft's compiler by the **Build USD Bridge** workflow and published to the `windows-usd-bridge`
release. Installer builds copy it to `bin/usdbridge` (solvusd.dll, usd_ms.dll, TBB, MSVC runtime and
the `usd` plugin folder). SOLVFX loads it at run time (Engine/Solv3D/SolvUsd.cpp).

Rebuild it only when the bridge or the USD version changes (workflow input `usd_tag`).
