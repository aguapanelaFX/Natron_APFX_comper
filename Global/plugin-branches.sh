# this file is sourced by the build-plugins.sh script if it exists
IO_BRANCH=master
MISC_BRANCH=master
ARENA_BRANCH=master
CV_BRANCH=master

# SOLVFX: plug-in sources pinned to the versions of the last good build (2026-11-12, run 43145521301).
# Upstream master moves (e.g. 2026-11-22/23: SupportExt headers moved, FFmpeg / defaults changes) and our
# patches in tools/jenkins/include/patches stop applying. Move these on purpose, after re-checking the patches.
IO_COMMIT=1bce1f1a2ad05c793682dd6845e62e5b352a3ea6
MISC_COMMIT=850b3fff65463c073bd64683e5d97a7dec2586c5
ARENA_COMMIT=49e4d5fc197a637196c1a7decab7f97751b867f6
GMIC_COMMIT=292ac55c7c84f6eff56c1764a5a5037b96895d12
# submodule pins (SOLVFX and plug-ins): tools/jenkins/checkout-repository.sh
