#!/usr/bin/env bash

set -euo pipefail

NVM_VERSION="${NVM_VERSION:-v0.40.3}"
NODE_MAJOR="${NODE_MAJOR:-22}"
PROJECT_PATH="${PROJECT_PATH:-$(pwd -P)}"

log() {
  printf '[codex-wsl] %s\n' "$*"
}

warn() {
  printf '[codex-wsl] WARN: %s\n' "$*" >&2
}

detect_windows_codex_home() {
  if ! command -v powershell.exe >/dev/null 2>&1; then
    return 1
  fi

  local windows_home windows_home_unix
  windows_home="$(
    powershell.exe -NoProfile -Command '$env:USERPROFILE' 2>/dev/null \
      | tr -d '\r' \
      | tail -n 1
  )"

  if [[ -z "${windows_home}" ]]; then
    return 1
  fi

  windows_home_unix="$(wslpath "${windows_home}")"
  if [[ -d "${windows_home_unix}/.codex" ]]; then
    printf '%s/.codex\n' "${windows_home_unix}"
    return 0
  fi

  return 1
}

ensure_nvm_loaded() {
  export NVM_DIR="${HOME}/.nvm"
  if [[ -s "${NVM_DIR}/nvm.sh" ]]; then
    # shellcheck source=/dev/null
    . "${NVM_DIR}/nvm.sh"
    return 0
  fi

  return 1
}

install_nvm_if_needed() {
  if ensure_nvm_loaded; then
    log "nvm already available"
    return 0
  fi

  log "installing nvm ${NVM_VERSION}"
  curl -fsSL "https://raw.githubusercontent.com/nvm-sh/nvm/${NVM_VERSION}/install.sh" | bash
  ensure_nvm_loaded
}

ensure_nvm_default_auto_use() {
  local bashrc_file="${HOME}/.bashrc"
  local marker="# codex-wsl: use the default nvm Node.js version"

  if grep -Fq "${marker}" "${bashrc_file}"; then
    log "nvm default auto-use already configured"
    return 0
  fi

  {
    printf '\n'
    printf '%s\n' "${marker}"
    printf 'if command -v nvm >/dev/null 2>&1; then\n'
    printf '  nvm use default >/dev/null 2>&1\n'
    printf 'fi\n'
  } >> "${bashrc_file}"

  log "enabled nvm default auto-use in ${bashrc_file}"
}

install_node_and_codex() {
  install_nvm_if_needed
  ensure_nvm_default_auto_use

  log "installing Node.js ${NODE_MAJOR}"
  nvm install "${NODE_MAJOR}"
  nvm alias default "${NODE_MAJOR}" >/dev/null
  nvm use default >/dev/null

  log "installing @openai/codex"
  npm install -g @openai/codex
}

copy_windows_codex_files() {
  local windows_codex_home="$1"
  local copied_any=0

  mkdir -p "${HOME}/.codex"

  for file_name in auth.json config.toml; do
    local source_file="${windows_codex_home}/${file_name}"
    local target_file="${HOME}/.codex/${file_name}"

    if [[ ! -f "${source_file}" ]]; then
      continue
    fi

    if [[ -f "${target_file}" ]]; then
      log "keeping existing ${target_file}"
      continue
    fi

    cp "${source_file}" "${target_file}"
    log "copied ${file_name} from Windows Codex home"
    copied_any=1
  done

  if [[ "${copied_any}" -eq 0 ]]; then
    log "no Windows Codex files copied"
  fi
}

ensure_project_trusted() {
  local config_file="${HOME}/.codex/config.toml"
  mkdir -p "${HOME}/.codex"

  if [[ ! -f "${config_file}" ]]; then
    : > "${config_file}"
  fi

  if grep -Fq "[projects.'${PROJECT_PATH}']" "${config_file}"; then
    log "project trust already configured for ${PROJECT_PATH}"
    return 0
  fi

  {
    printf '\n'
    printf "[projects.'%s']\n" "${PROJECT_PATH}"
    printf 'trust_level = "trusted"\n'
  } >> "${config_file}"

  log "marked ${PROJECT_PATH} as trusted"
}

main() {
  local windows_codex_home="${WINDOWS_CODEX_HOME:-}"

  if [[ -z "${windows_codex_home}" ]]; then
    windows_codex_home="$(detect_windows_codex_home || true)"
  fi

  install_node_and_codex

  if [[ -n "${windows_codex_home}" && -d "${windows_codex_home}" ]]; then
    copy_windows_codex_files "${windows_codex_home}"
  else
    warn "Windows Codex home not found; skipped config/auth import"
  fi

  ensure_project_trusted

  log "done"
  log "open a new shell or run: source ~/.bashrc"
  log "verify with: codex --version"
}

main "$@"
