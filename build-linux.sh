#!/usr/bin/env bash
#
# Convenience script for building on Linux.

function main() {
  set -e

  local build_dir="${PWD}/build"
  local build_mode="Release"
  local install_dir="${PWD}/install"
  local target="eoserv"
  local opt_clean="false"
  local opt_help="false"
  local opt_test="false"
  local mariadb="OFF"
  local sqlite="OFF"
  local sqlserver="ON"

  local option
  while [[ "$#" -gt 0 ]]
  do
    option="$1"
    case "${option}" in
      -d|--debug)
        build_mode="Debug"
        ;;
      -c|--clean)
        opt_clean="true"
        ;;
      -b|--build_dir)
        build_dir="$2"
        shift
        ;;
      -i|--install)
        target="install"
        ;;
      -t|--test)
        opt_test="true"
        ;;
      -h|--help)
        opt_help="true"
        break
        ;;
      --sqlite)
        sqlite="$2"
        shift
        ;;
      --maria)
        sqlite="$2"
        shift
        ;;
      --sqlserver)
        sqlserver="$2"
        shift
        ;;

      *)
        echo "Error: unsupported option \"${option}\""
        return 1
        ;;
    esac
    shift
  done

  if [[ "${opt_help}" == "true" ]]; then
    display_usage
    return 0
  fi

  if [[ -z "${build_dir}" ]]; then
    echo "Error: a directory must be specified with option -b|--build_dir"
    return 1
  fi

  if [[ "${mariadb}" != "ON" && "${mariadb}" != "OFF" ]]; then
    echo "Error: acceptable values for option --mariadb are ON|OFF"
    return 1
  fi

  if [[ "${sqlite}" != "ON" && "${sqlite}" != "OFF" ]]; then
    echo "Error: acceptable values for option --sqlite are ON|OFF"
    return 1
  fi

  if [[ "${sqlserver}" != "ON" && "${sqlserver}" != "OFF" ]]; then
    echo "Error: acceptable values for option --sqlserver are ON|OFF"
    return 1
  fi

  echo ""
  echo "Build mode: ${build_mode}"
  echo "Build directory: ${build_dir}"
  echo "Install directory: ${install_dir}"
  echo "MariaDB support: ${mariadb}"
  echo "SQLite support: ${sqlite}"
  echo "SQL Server support: ${sqlserver}"

  if [[ "${opt_clean}" == "true" ]]; then
    echo ""
    echo "Cleaning build and install directories"
    if [[ -d "${build_dir}" ]]; then
      rm -rf "${build_dir}"
    fi
    if [[ -d "${install_dir}" ]]; then
      rm -rf "${install_dir}"
    fi
  fi

  if [[ ! -d "${build_dir}" ]]; then
    mkdir -p "${build_dir}"
  fi

  local db_engine_macros=""
  db_engine_macros+="-DEOSERV_WANT_MYSQL=${mariadb}"
  db_engine_macros+=" -DEOSERV_WANT_SQLITE=${sqlite}"
  db_engine_macros+=" -DEOSERV_WANT_SQLSERVER=${sqlserver}"

  pushd "${build_dir}" > /dev/null

  echo ""
  cmake "${db_engine_macros}" -G "Unix Makefiles" ..
  cmake --build . --config "${build_mode}" --target "${target}"

  popd > /dev/null

  if [[ "${opt_test}" == "true" ]]; then
      echo "./install/test/eoserv_test"
  fi

  return 0
}

function display_usage() {
  echo "Usage:"
  echo "  build-linux.sh [options]"
  echo ""
  echo "Options:"
  echo "  -d --debug                  Build with debug symbols."
  echo "  -c --clean                  Clean before building."
  echo "  -b <dir> --build_dir <dir>  Build directory [default: build]."
  echo "  -i --install                Install build in install directory."
  echo "  --mariadb (ON|OFF)          MariaDB/MySQL support [default: OFF]."
  echo "  --sqlite (ON|OFF)           SQLite support [default: OFF]."
  echo "  --sqlserver (ON|OFF)        SQL Servier support [default: ON]."
  echo "  -h --help                   Display this message."
}

main "$@"
