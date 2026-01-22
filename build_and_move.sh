#!/bin/bash

# 遇到错误立即停止
set -e

# ==========================================
# 【路径配置】
# 1. Windows 共享文件夹挂载点
# 注意：Linux区分大小写，请确保 UbuntuShare 的大小写与 ls /mnt/hgfs/ 查到的一致
TARGET_SHARED_DIR="/mnt/hgfs/ubuntuShare"

# 2. 共享文件夹里的父级目录名称
SHARED_PARENT_NAME="MNN_package"
# ==========================================

echo "========================================"
echo "   MNN Android 编译 & 同步脚本"
echo "   (运行位置: MNN_Perfetto 根目录)"
echo "========================================"

# --- 1. 定义路径变量 ---
# 脚本当前所在目录 (即 MNN_Perfetto)
PROJECT_ROOT=$(pwd)

# 编译工作的具体目录 (必须在这个目录下执行 build_64.sh)
BUILD_WORK_DIR="$PROJECT_ROOT/project/android/build_64"

# 模型的源目录
MODEL_SOURCE_DIR="$PROJECT_ROOT/transformers/llm/export/model"

# 本地打包输出目录 (现在会生成在 MNN_Perfetto/android_demo_package)
PACKAGE_NAME="android_demo_package"
OUTPUT_DIR="$PROJECT_ROOT/$PACKAGE_NAME"

# 最终远程目标路径
FINAL_DEST_PARENT="$TARGET_SHARED_DIR/$SHARED_PARENT_NAME"

# --- 2. 检查环境 ---
# 检查共享文件夹
if [ ! -d "$TARGET_SHARED_DIR" ]; then
    echo "❌ 错误: 找不到共享文件夹路径: $TARGET_SHARED_DIR"
    echo "   请检查挂载命令，并确认文件夹名称大小写 (UbuntuShare vs ubuntuShare)。"
    exit 1
fi

# --- 3. 进入编译目录执行编译 ---
echo ""
echo ">>> [1/4] 进入构建目录并开始编译..."

# 确保编译目录存在
mkdir -p "$BUILD_WORK_DIR"

# 关键步骤：切换工作目录到 build_64
cd "$BUILD_WORK_DIR"

# 执行编译 (调用上一级的 build_64.sh)
# 注意：这里的路径是相对于 BUILD_WORK_DIR 的
../build_64.sh "-DMNN_LOW_MEMORY=true -DMNN_CPU_WEIGHT_DEQUANT_GEMM=true -DMNN_BUILD_LLM=true -DMNN_SUPPORT_TRANSFORMER_FUSE=true -DMNN_ARM82=true -DMNN_OPENCL=true -DMNN_USE_LOGCAT=true -DMNN_BUILD_DEMO=ON -DCMAKE_CXX_STANDARD=17 -DANDROID_PLATFORM=android-29"

# 检查产物 (在 build_64 目录下检查)
if [ ! -f "libllm.so" ]; then
    echo "❌ 错误: 编译产物 libllm.so 未找到，编译可能失败。"
    # 退出前切回原目录（虽然后面exit了，但是好习惯）
    cd "$PROJECT_ROOT"
    exit 1
fi

# --- 4. 整理文件 (从 build_64 复制出来) ---
echo ""
echo ">>> [2/4] 正在整理文件到本地: $OUTPUT_DIR ..."

# 暂时切回项目根目录来操作输出文件夹
cd "$PROJECT_ROOT"

if [ -d "$OUTPUT_DIR" ]; then
    rm -rf "$OUTPUT_DIR"
fi
mkdir -p "$OUTPUT_DIR/model_dir"

# 注意：源文件在 BUILD_WORK_DIR 里面
cp "$BUILD_WORK_DIR/libMNN.so" "$OUTPUT_DIR/"
cp "$BUILD_WORK_DIR/libMNN_Express.so" "$OUTPUT_DIR/"
cp "$BUILD_WORK_DIR/libllm.so" "$OUTPUT_DIR/"
if [ -f "$BUILD_WORK_DIR/libMNN_CL.so" ]; then cp "$BUILD_WORK_DIR/libMNN_CL.so" "$OUTPUT_DIR/"; fi
cp "$BUILD_WORK_DIR/llm_demo" "$OUTPUT_DIR/"
cp "$BUILD_WORK_DIR/llm_bench" "$OUTPUT_DIR/"

# 复制模型文件
if [ "$(ls -A $MODEL_SOURCE_DIR 2>/dev/null)" ]; then
    cp "$MODEL_SOURCE_DIR"/* "$OUTPUT_DIR/model_dir/"
else
    echo "⚠️ 警告: 模型目录为空，生成的包里将不包含模型文件。"
fi

# --- 5. 复制到共享文件夹 ---
echo ""
echo ">>> [3/4] 正在处理共享文件夹目录结构..."

# 确保 /mnt/hgfs/UbuntuShare/MNN_package 存在
if [ ! -d "$FINAL_DEST_PARENT" ]; then
    echo "    创建目录: $FINAL_DEST_PARENT"
    mkdir -p "$FINAL_DEST_PARENT"
fi

# 删除旧版本
if [ -d "$FINAL_DEST_PARENT/$PACKAGE_NAME" ]; then
    echo "    删除旧版本..."
    rm -rf "$FINAL_DEST_PARENT/$PACKAGE_NAME"
fi

echo ">>> [4/4] 正在复制到: $FINAL_DEST_PARENT ..."
cp -r "$OUTPUT_DIR" "$FINAL_DEST_PARENT/"

echo ""
echo "========================================"
echo "✅ 全部完成！"
echo "本地路径: $OUTPUT_DIR"
echo "共享路径: $FINAL_DEST_PARENT/$PACKAGE_NAME"
echo "========================================"
