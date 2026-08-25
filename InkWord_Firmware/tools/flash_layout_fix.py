"""Flash 布局修正脚本（platformio.ini extra_scripts，2026-08-25 真机 boot loop 根治）

partitions_default.csv 把 nvs 扩到 192KB（T1.5 全词库 LR 持久化），
factory 顺移 0x40000、otadata 在 0x940000；PIO/Arduino 工具链按默认
布局两处错位：
  1. ESP32_APP_OFFSET 兜底 0x10000 → app 烧进 nvs/phy 区，bootloader
     在 0x40000 读到 app 镜像中段随机数据，校验失败静默复位循环
  2. boot_app0.bin 硬编码烧 0xe000（默认布局 otadata 位）→ 落在
     nvs 分区内造成污染

本脚本钉死：app → factory 0x40000；boot_app0 → otadata 0x940000。
分区布局若再变更（partitions_default.csv），同步改下方两常量。
"""

Import("env")

FACTORY_OFFSET = "0x40000"    # factory, app, 0x40000, 0x300000
OTADATA_OFFSET = "0x940000"   # otadata, data, ota, 0x940000, 0x2000

env.Replace(ESP32_APP_OFFSET=FACTORY_OFFSET)

# 层 1：条目尚在 FLASH_EXTRA_IMAGES（列表阶段）则改写列表
fixed = []
for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
    if str(offset).lower() == "0xe000":
        offset = OTADATA_OFFSET  # boot_app0.bin → 真实 otadata 分区
    fixed.append((offset, image))
if fixed:
    env.Replace(FLASH_EXTRA_IMAGES=fixed)

# 层 2：平台已把 extra images 展平进 UPLOADERFLAGS
# （["...", "0xe000", ".../boot_app0.bin", ...]），直接扫平铺列表替换字面量
flags = list(env.get("UPLOADERFLAGS", []))
if "0xe000" in [str(f).lower() for f in flags]:
    flags = [OTADATA_OFFSET if str(f).lower() == "0xe000" else f for f in flags]
    env.Replace(UPLOADERFLAGS=flags)

print("flash_layout_fix: app -> %s, boot_app0(0xe000) -> %s" % (FACTORY_OFFSET, OTADATA_OFFSET))
