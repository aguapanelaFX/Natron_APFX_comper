#!/bin/bash

NATRON_DIR=
NATRON_REPO_DIR=
if [[ -z $1 || -z $2 ]]; then
  echo "Usage: $(basename $0) <natron_directory> <natron_repo_directory>"
  exit 1
else
  NATRON_DIR=$(realpath $1)
  NATRON_REPO_DIR=$(realpath $2)
fi

source ${NATRON_DIR}/tools/MINGW-packages/natron_repo_common.sh

if [[ ! -d ${NATRON_REPO_DIR} ]]; then
    mkdir ${NATRON_REPO_DIR}
fi

cd ${NATRON_REPO_DIR}

if [[ "${MSYSTEM:-}" == "MINGW64" ]]; then
    # Upstream Natron MINGW64 package repo.
    WINDOWS_PACMAN_REPO_VERSION=`cat ${NATRON_DIR}/tools/MINGW-packages/windows_pacman_repo_version.txt`
    ZIP_FILENAME="natron_package_repo-${WINDOWS_PACMAN_REPO_VERSION}.zip"
    ZIP_URL="https://github.com/NatronGitHub/Natron/releases/download/windows-mingw-package-repo/${ZIP_FILENAME}"
else
    # UCRT64 package repo published by this repository's "Build pacman repo" workflow.
    REPO_SLUG="${NATRON_PACMAN_REPO_SLUG:-${GITHUB_REPOSITORY:-aguapanelaFX/Natron_APFX_comper}}"
    ZIP_FILENAME="natron_package_repo-${MSYSTEM,,}.zip"
    ZIP_URL="https://github.com/${REPO_SLUG}/releases/download/windows-${MSYSTEM,,}-package-repo/${ZIP_FILENAME}"
fi
wget "${ZIP_URL}" -O "${ZIP_FILENAME}" || rm -f "${ZIP_FILENAME}"

if [[ -s ${ZIP_FILENAME} ]]; then
    unzip -o ${ZIP_FILENAME}
else
    echo "Failed to fetch ${ZIP_URL}"

    if [[ -n "${CI:-}" ]]; then
        echo "ERROR: the Natron ${MSYSTEM} package repo is not published yet."
        echo "Run the 'Build pacman repo' workflow first, then re-run this build."
        exit 1
    fi

    echo "Building pacman repo locally."

    # install necessary dependencies for building pacman repo.
    pacman -S --needed --noconfirm git base-devel

    ${NATRON_DIR}/tools/MINGW-packages/build_natron_package_repo.sh ${NATRON_REPO_DIR}
fi

UNIX_NATRON_REPO_DIR=`cygpath -u ${NATRON_REPO_DIR}`
natron_repo_init ${UNIX_NATRON_REPO_DIR}
