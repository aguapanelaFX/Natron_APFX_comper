#!/bin/bash

set -e # Exit immediately if a command exits with a non-zero status
set -u # Treat unset variables as an error when substituting.
#set -x # Print commands and their arguments as they are executed.

# GIT_COMMIT=xxxx If doing a ci or snapshot build, must be set to the commit to build
# GIT_BRANCH=xxxx Used if GIT_COMMIT is not set, it points to the branch (or tag if BUILD_CONFIG=STABLE) to build
# REPO: Must point to the git URL of the Natron repo to use

source common.sh
source manageBuildOptions.sh
source manageLog.sh

updateBuildOptions

function checkoutRepository() {
    REPO_URL="$1"
    REPO_LOCAL_DIRNAME="$2"
    REPO_BRANCH="$3"
    REPO_COMMIT="$4"
    COMMIT_BUILD_OPTION_NAME="$5"
    TAR_SOURCES=$6
    if [ -z "$REPO_URL" ]; then
        echo "Please define REPO"
        return 1
    fi

    printStatusMessage "===> Cloning $REPO_URL on branch $REPO_BRANCH, commit $REPO_COMMIT"

    if [ "${DEBUG_SCRIPTS:-}" != "1" ] && [ -d "$TMP_PATH/$REPO_LOCAL_DIRNAME" ]; then
        rm -rf "$TMP_PATH/$REPO_LOCAL_DIRNAME"
    fi

    # clone
    if [ ! -d "$TMP_PATH" ]; then
        mkdir -p "$TMP_PATH"
    fi
    cd "$TMP_PATH"
    if [ -z "$REPO_COMMIT" ]; then
        branch="$REPO_BRANCH"
    else
        branch="$REPO_COMMIT"
    fi
    echo branch=$branch BRANCH=$REPO_BRANCH COMMIT=$REPO_COMMIT
    deep=false
    if [ ! -d "$REPO_LOCAL_DIRNAME" ] && [ -n "$REPO_COMMIT" ]; then
        # SOLVFX: pinned commit (clone -b does not take a commit): fetch just that commit
        $GIT init -q "$REPO_LOCAL_DIRNAME"
        (cd "$REPO_LOCAL_DIRNAME" && $GIT remote add origin "$REPO_URL" && \
         $TIMEOUT 1800 $GIT fetch -q --depth 1 origin "$REPO_COMMIT" && $GIT checkout -q FETCH_HEAD) || return 1
        echo "SOLVFX: $REPO_LOCAL_DIRNAME pinned at $REPO_COMMIT"
    fi
    if [ ! -d "$REPO_LOCAL_DIRNAME" ]; then
        # slow version:
        if "$deep" && [ -z "$REPO_COMMIT" ]; then
            $TIMEOUT 1800 $GIT clone "$REPO_URL" -b "${REPO_BRANCH}" "$REPO_LOCAL_DIRNAME"
        else
            # fast version (depth=1):
            $TIMEOUT 1800 $GIT clone --depth 1 -b "${branch#tags/}" "$REPO_URL" "$REPO_LOCAL_DIRNAME"
        fi
    fi

    cd "$REPO_LOCAL_DIRNAME"

    if [ -z "$REPO_COMMIT" ]; then
        REPO_COMMIT=$(git rev-parse HEAD)
        echo "done, got commit $REPO_COMMIT"
    else
        $GIT checkout -q "$REPO_COMMIT" 2>/dev/null || $GIT checkout "$REPO_COMMIT"
    fi


    # the snapshot are always built with latest version
    # slow version:
    #$TIMEOUT 1800 $GIT submodule update --init --recursive --remote
    # fast version (depth=1):
    $TIMEOUT 1800 $GIT submodule update --depth 1 --init --recursive --remote

    # SOLVFX: submodules pinned to the versions of the last good build (2026-11-12, run 43145521301).
    # "--remote" above takes the newest upstream commit of every submodule; upstream changes on 2026-11-22/23
    # broke the build (openfx cursor names, SupportExt headers, SequenceParsing). Pins apply to SOLVFX (Natron)
    # and to the plug-ins. Move a pin on purpose only, after a test build.
    case "$REPO_LOCAL_DIRNAME" in
    Natron|openfx-*)
        export SOLVFX_PIN_SUPPORTEXT="${SOLVFX_PIN_SUPPORTEXT:-2485505dbe55885ee92ca9f164e7732a85856a13}"
        export SOLVFX_PIN_OPENFX="${SOLVFX_PIN_OPENFX:-0759758d0fb57932b5c4698fff9ef6b4a3132309}"
        export SOLVFX_PIN_OPENFX_IO="${SOLVFX_PIN_OPENFX_IO:-1bce1f1a2ad05c793682dd6845e62e5b352a3ea6}"
        export SOLVFX_PIN_SEQUENCEPARSING="${SOLVFX_PIN_SEQUENCEPARSING:-3c93fcc488632b0bdfeee3181586809932357598}"
        export SOLVFX_PIN_TINYDIR="${SOLVFX_PIN_TINYDIR:-64fb1d4376d7580aa1013fdbacddbbeba67bb085}"
        export SOLVFX_PIN_LODEPNG="${SOLVFX_PIN_LODEPNG:-7b2893e35891c0c85bee5595caafaf3c2044b409}"
        export SOLVFX_PIN_BREAKPAD="${SOLVFX_PIN_BREAKPAD:-9474c3f7f9939391f281d46c42bfe20cc0f0abd9}"
        $GIT submodule foreach --recursive '
            pin=""
            case "$(basename "$sm_path")" in
            SupportExt) pin="$SOLVFX_PIN_SUPPORTEXT" ;;
            openfx|OpenFX) pin="$SOLVFX_PIN_OPENFX" ;;
            OpenFX-IO) pin="$SOLVFX_PIN_OPENFX_IO" ;;
            SequenceParsing) pin="$SOLVFX_PIN_SEQUENCEPARSING" ;;
            tinydir) pin="$SOLVFX_PIN_TINYDIR" ;;
            lodepng) pin="$SOLVFX_PIN_LODEPNG" ;;
            google-breakpad) pin="$SOLVFX_PIN_BREAKPAD" ;;
            esac
            if [ -n "$pin" ] && [ "$(git rev-parse HEAD)" != "$pin" ]; then
                { git fetch -q --depth 1 origin "$pin" && git checkout -q "$pin"; } || { echo "SOLVFX: cannot pin $sm_path at $pin"; exit 1; }
                echo "SOLVFX: $sm_path pinned at $pin"
                git submodule update -q --init --recursive --depth 1 || true
            fi
        ' || return 1
        ;;
    esac

    # copy tarball of sources pruned of git files
    if [ "${TAR_SOURCES:-}" = "1" ]; then
        SRC_DIR_NAME="$REPO_LOCAL_DIRNAME-$REPO_BRANCH-$REPO_COMMIT"
        cp -a "$REPO_LOCAL_DIRNAME" "$SRC_DIR_NAME"
        (cd "$SRC_DIR_NAME"; find . -type d -name .git -exec rm -rf {} \;)
        tar cvvJf "$SRC_PATH/$SRC_DIR_NAME.tar.xz" "$SRC_DIR_NAME"
        rm -rf "$SRC_DIR_NAME"
    fi

    if [ -n "$REPO_COMMIT" ] && [ -n "$COMMIT_BUILD_OPTION_NAME" ]; then
        setBuildOption "$COMMIT_BUILD_OPTION_NAME" "$REPO_COMMIT"
    fi

    cd $CWD
    return 0
}

# Local variables:
# mode: shell-script
# sh-basic-offset: 4
# sh-indent-comment: t
# indent-tabs-mode: nil
# End:
