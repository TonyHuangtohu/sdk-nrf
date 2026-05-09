# nRF54L10 DFU Target Support Notes

## 修改目标

为官方 `samples/dfu/dfu_target` 示例新增 `nrf54l15dk/nrf54l10/cpuapp` 构建支持，使 `build_dfu_target_3.2.3_nRF54L10.bat` 可以成功构建。

## 主要修改

1. 新增 `pm_static_nrf54l15dk_nrf54l10_cpuapp.yml`

   该文件是 `nrf54l15dk/nrf54l10/cpuapp` 专用的 Partition Manager 静态分区配置。NCS 会根据 board target 自动查找这个文件名。

2. 为 nRF54L10 重新规划内部 RRAM 分区

   nRF54L10 的 APP RRAM 容量小于 nRF54L15，不能直接复用 `pm_static_nrf54l15dk_nrf54l15_cpuapp.yml` 中的 L15 分区布局。

   新增布局保留了示例所需的三个核心区域：

   - `mcuboot_primary`: `0xe000` - `0x5d000`, size `0x4f000`
   - `mcuboot_secondary`: `0x5d000` - `0xac000`, size `0x4f000`
   - `dfu_target_helper`: `0xac000` - `0xfb000`, size `0x4f000`

3. 更新 `sample.yaml`

   在 `platform_allow` 和 `integration_platforms` 中加入：

   ```yaml
   - nrf54l15dk/nrf54l10/cpuapp
   ```

4. 更新 `README.rst`

   补充 nRF54L10 的 `dfu_target_helper` 地址：

   ```text
   nRF54L10 DK: 0xac000
   ```

## dfu_target_helper 分区作用

`dfu_target_helper` 是该示例额外预留的临时镜像存放区。因为该 sample 没有实现 UART、BLE、MCUmgr 等传输层，所以测试 DFU 时需要先把新固件镜像手动烧录到 `dfu_target_helper`。

运行时，shell 命令会从 `dfu_target_helper` 读取镜像内容，再通过 DFU target API 写入 `mcuboot_secondary`，最后由 MCUboot 在重启后完成升级。

## 构建验证

已运行：

```bat
build_dfu_target_3.2.3_nRF54L10.bat
```

构建结果成功，已生成以下关键产物：

- `build_54l10\merged.hex`
- `build_54l10\dfu_application.zip`
- `build_54l10\dfu_target\zephyr\zephyr.signed.bin`

生成的 `pm_config.h` 中确认：

```c
#define PM_DFU_TARGET_HELPER_ADDRESS 0xac000
#define PM_DFU_TARGET_HELPER_SIZE 0x4f000
```
