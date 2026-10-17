#!/bin/sh
set -e
#set -x
BUILD_SCRIPT_DIR=$(realpath $(dirname "${BASH_SOURCE[0]}"))

# Natron-specific packages only. Everything else (FFmpeg, OpenImageIO, OpenColorIO,
# OpenEXR, Imath, LibRaw, Poppler, SoX, ...) comes from MSYS2's own, maintained packages.
# - seexpr-git: openfx-io needs the SeExpr v1 API (MSYS2 ships SeExpr v2)
# - imagemagick: openfx-arena is built against ImageMagick 6
PKGS="
mingw-w64-natron-setup
mingw-w64-seexpr-git
mingw-w64-imagemagick
mingw-w64-osmesa
mingw-w64-dump_syms
mingw-w64-natron-build-deps-common
mingw-w64-natron-build-deps-qt5
mingw-w64-natron-build-deps-qt6
"

source "${BUILD_SCRIPT_DIR}/natron_repo_common.sh"

trap natron_repo_cleanup EXIT

NATRON_REPO_DIR=
if [[ -z $1 ]]; then
  echo "Usage: $(basename $0) <natron_repo_directory>"
  exit 1
else
  NATRON_REPO_DIR=$(realpath $1)
fi

natron_repo_init ${NATRON_REPO_DIR}

# Package prefix of the current MSYS2 environment (mingw-w64-x86_64 for MINGW64,
# mingw-w64-ucrt-x86_64 for UCRT64, ...). Build only for this environment.
PKG_PREFIX="${MINGW_PACKAGE_PREFIX:-mingw-w64-x86_64}"
export MINGW_ARCH="${MSYSTEM,,}"
echo "Building Natron package repo for ${MSYSTEM} (prefix ${PKG_PREFIX})"

# Keep building the remaining packages when one fails, so all failures show up
# in a single run; the script still exits with an error at the end.
FAILED_PKGS=""
# Packages (re)built during this run; packages depending on them are rebuilt too.
REBUILT_PKGS=" "
# Packages that belong in the repo; anything else left over from earlier runs is removed.
EXPECTED_PKGS=" "

# A package already in the repo is reused only if its recipe (the git-tracked
# files of its directory) is unchanged since it was built and none of its
# dependencies were rebuilt in this run. Recipe hashes are kept next to the repo.
RECIPE_HASH_DIR="${NATRON_REPO_DIR}/.recipes"
mkdir -p "${RECIPE_HASH_DIR}"

recipe_hash() {
  (cd "$1" && git ls-files -z . | sort -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')
}

in_natron_repo() {
  pacman -Sl natron 2>/dev/null | awk '{print $2}' | grep -qx "$1"
}

depends_on_rebuilt() {
  local dep
  for dep in $(makepkg --printsrcinfo | awk '/^[[:space:]]*(depends|makedepends) = /{print $3}' | sed -E 's/[<>=].*$//'); do
    if [[ "${REBUILT_PKGS}" == *" ${dep} "* ]]; then
      echo "${dep}"
      return 0
    fi
  done
  return 1
}

# Remove packages that are no longer part of the repo (e.g. dropped recipes)
# before building, so they can't interfere with dependency resolution.
for pkg_dir in ${PKGS}; do
  EXPECTED_PKGS="${EXPECTED_PKGS}$(cd "${BUILD_SCRIPT_DIR}/${pkg_dir}" && makepkg --printsrcinfo | awk '/pkgname/{print $3}') "
done
for old_pkg in $(pacman -Sl natron 2>/dev/null | awk '{print $2}'); do
  if [[ "${EXPECTED_PKGS}" != *" ${old_pkg} "* ]]; then
    echo "Removing ${old_pkg} from package repo (no longer built)."
    (cd "${NATRON_REPO_DIR}" && repo-remove natron.db.tar.gz "${old_pkg}" && rm -f "${old_pkg}"-[0-9]*-any.pkg.tar.* ".recipes/${old_pkg}.sha256")
  fi
done
pacman -Sy

for pkg_dir in ${PKGS}; do
  echo -e "\n\nEntering package directory ${pkg_dir}."
  cd "${BUILD_SCRIPT_DIR}/${pkg_dir}"

  PACKAGE_NAME=`makepkg --printsrcinfo | awk '/pkgname/{print $3}'`

  if [[ ${PACKAGE_NAME} == *-osmesa && ${PKG_PREFIX} != 'mingw-w64-x86_64' ]]; then
    echo "Skipping ${PACKAGE_NAME}: only available as a prebuilt MINGW64 package."
    cd "${BUILD_SCRIPT_DIR}"
    continue
  fi

  RECIPE_HASH="$(recipe_hash .)"
  HASH_FILE="${RECIPE_HASH_DIR}/${PACKAGE_NAME}.sha256"
  REASON=""
  if ! in_natron_repo "${PACKAGE_NAME}"; then
    REASON="not in repository yet"
  elif [[ ! -f "${HASH_FILE}" || "$(cat "${HASH_FILE}")" != "${RECIPE_HASH}" ]]; then
    REASON="recipe changed"
  elif CHANGED_DEP=$(depends_on_rebuilt); then
    REASON="dependency ${CHANGED_DEP} was rebuilt"
  fi

  if [[ -z "${REASON}" ]]; then
    echo "Skipping. ${PACKAGE_NAME} already in repository and up to date."
  else
    echo "Building ${PACKAGE_NAME} (${REASON}) ..."

    # Remove any package files from previous builds.
    rm -f ${PKG_PREFIX}-*-any.pkg.tar.*

    if [[ ${PACKAGE_NAME} == 'mingw-w64-x86_64-osmesa' ]]; then
      echo "Fetching prebuilt ${PACKAGE_NAME} ..."
      # Can't build osmesa right now so use previously built package.
      OSMESA_PKG_FILENAME=mingw-w64-x86_64-osmesa-111.471db69.17.1.10.3.9.1-2-any.pkg.tar.xz
      wget -O ${OSMESA_PKG_FILENAME} https://sourceforge.net/projects/natron/files/MINGW-packages/mingw64/${OSMESA_PKG_FILENAME}/download
      echo "Fetch complete."
    else
      # Print available disk space to facilitate GitHub action debugging.
      df -h

      if ! time MAKEFLAGS="-j$(nproc)" makepkg-mingw -LfCcsr --needed --noconfirm; then
        echo "ERROR: building ${PACKAGE_NAME} failed, continuing with the next package."
        FAILED_PKGS="${FAILED_PKGS} ${PACKAGE_NAME}"
        cd "${BUILD_SCRIPT_DIR}"
        continue
      fi

      echo "Build complete."
    fi

    # Drop older builds of this package from the repo directory.
    rm -f "${NATRON_REPO_DIR}/${PACKAGE_NAME}"-[0-9]*-any.pkg.tar.*

    for pkg_file in `find . -name "${PKG_PREFIX}-*-any.pkg.tar.*"`; do
      echo "Adding ${pkg_file} to package repo ..."
      natron_repo_add_package ${pkg_file}
    done
    cd "${BUILD_SCRIPT_DIR}/${pkg_dir}"
    echo "${RECIPE_HASH}" > "${HASH_FILE}"
    REBUILT_PKGS="${REBUILT_PKGS}${PACKAGE_NAME} "
  fi

  echo -e "Leaving package directory ${pkg_dir}."
  cd "${BUILD_SCRIPT_DIR}"
done

if [[ -n "${FAILED_PKGS}" ]]; then
  echo -e "\n\nERROR: the following packages failed to build:${FAILED_PKGS}"
  exit 1
fi
echo -e "\n\nAll packages built successfully."
