# WSL2 Codex Setup

在 Ubuntu WSL 中执行：

```bash
cd /home/hefeng/MNN_WSL2
bash tools/script/setup_codex_wsl.sh
source ~/.bashrc
codex --version
```

脚本会做这些事：

- 用 `nvm` 安装 Linux 版 Node.js。
- 自动在新开 WSL shell 中切换到 `nvm default`，避免误用 Windows 挂载盘上的旧 `node/npm/codex`。
- 用 `npm install -g @openai/codex` 安装 Codex CLI。
- 如果检测到 Windows 侧 `%USERPROFILE%\\.codex`，自动复制 `auth.json` 和 `config.toml` 到 WSL 的 `~/.codex`。
- 把当前项目路径加入 Codex trusted project 列表，避免每次重复确认。

可选环境变量：

```bash
WINDOWS_CODEX_HOME=/mnt/c/Users/<YourUser>/.codex
NODE_MAJOR=22
PROJECT_PATH=/home/hefeng/MNN_WSL2
bash tools/script/setup_codex_wsl.sh
```

如果你已经在 Windows 侧登录过 Codex，这套方式通常不需要在 WSL 里再次登录。
