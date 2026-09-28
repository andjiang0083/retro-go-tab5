# This file is injected late into rg_tool.py, you can run arbitrary python code here

# Espressif chip in the device
IDF_TARGET = "esp32p4"
# 只编 MVP 需要的两个 app（多机种大全不长在 16MB flash 上，ROM 全在 SD 卡）
DEFAULT_APPS = "launcher gbsp"
