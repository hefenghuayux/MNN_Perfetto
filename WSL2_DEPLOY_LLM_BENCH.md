# MNN `llm_bench` in WSL2

This workspace copy is prepared for Linux x86 in WSL2, not Android/ADB.

## 1. Make sure WSL2 has an Ubuntu distro

In PowerShell:

```powershell
wsl --install Ubuntu
```

If Ubuntu is already installed, start it:

```powershell
wsl -d Ubuntu
```

## 2. Install build dependencies inside WSL2

```bash
sudo apt update
sudo apt install -y build-essential cmake git
```

## 3. Build `llm_bench`

If you work directly on the Windows-mounted path:

```bash
cd /mnt/e/workspacce/WSL2/MNN_WSL2
chmod +x build_llm_bench_wsl.sh run_llm_bench_wsl.sh
./build_llm_bench_wsl.sh
```

For better compile speed, you can also copy the repo into the Linux filesystem first.

## 4. Run `llm_bench`

```bash
cd /mnt/e/workspacce/WSL2/MNN_WSL2
./run_llm_bench_wsl.sh
```

Useful overrides:

```bash
THREADS=8 ./run_llm_bench_wsl.sh
PREFILL_CPU_IDS=4,5,6,7 DECODE_CPU_IDS=0,1,2,3 ./run_llm_bench_wsl.sh
MODEL_DIR=/path/to/full/exported/model ./run_llm_bench_wsl.sh
```

## 5. Note about the current model files

The copied `model_dir` currently contains:

- `config.json`
- `llm.mnn`
- `llm.mnn.json`
- `llm_config.json`
- `tokenizer.txt`

`config.json` points to `llm.mnn.weight`, but that file is not present in the copied model artifacts. If the runtime fails to load, use a complete exported model directory or adjust `config.json` to match the real model layout.
