#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:-}

if [[ -z "$REPO_ROOT" ]]; then
  REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fi

if [[ -n "${MINIKV_PYTHON:-}" ]]; then
  if [[ ! -x "$MINIKV_PYTHON" ]]; then
    echo "MINIKV_PYTHON is not executable: $MINIKV_PYTHON" >&2
    exit 1
  fi
  echo "$MINIKV_PYTHON"
  exit 0
fi

if ! command -v uv >/dev/null 2>&1; then
  echo "missing required command: uv" >&2
  echo "install uv or set MINIKV_PYTHON to an existing test Python" >&2
  exit 1
fi

export UV_CACHE_DIR="${UV_CACHE_DIR:-$REPO_ROOT/.cache/uv}"
export UV_PROJECT_ENVIRONMENT="${UV_PROJECT_ENVIRONMENT:-$REPO_ROOT/.venv}"

uv sync --project "$REPO_ROOT" --quiet

PYTHON_BIN="$UV_PROJECT_ENVIRONMENT/bin/python3"
if [[ ! -x "$PYTHON_BIN" ]]; then
  PYTHON_BIN="$UV_PROJECT_ENVIRONMENT/bin/python"
fi

if [[ ! -x "$PYTHON_BIN" ]]; then
  echo "uv did not create an executable Python at $UV_PROJECT_ENVIRONMENT" >&2
  exit 1
fi

echo "$PYTHON_BIN"
