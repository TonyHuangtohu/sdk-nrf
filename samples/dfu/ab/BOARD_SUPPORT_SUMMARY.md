# DFU A/B Board Support Summary

本文总结在 `samples/dfu/ab` 官方 A/B with MCUboot 例程中，为 `nrf54lm20dk/nrf54lm20a/cpuapp` 和 `nrf54l15dk/nrf54l10/cpuapp` 增加支持时涉及的主要修改点。

测试和验证环境仅限于 `NCS v3.2.3`。本文中的构建结果、分区布局、配置项和地址计算均基于该版本，不代表其他 NCS 版本中的行为。

## 背景

官方例程原本支持：

- `nrf54h20dk/nrf54h20/cpuapp`
- `nrf54l15dk/nrf54l15/cpuapp`

当前已扩展支持：

- `nrf54lm20dk/nrf54lm20a/cpuapp`
- `nrf54l15dk/nrf54l10/cpuapp`

两个新增 target 的共同目标是让应用和 MCUboot 都能使用 retained memory 传递 boot request，从而支持 Direct XIP with revert 模式下的 A/B slot 切换、确认和回滚流程。

## 公共入口修改

`sample.yaml` 中将两个新 target 加入 `sample.dfu.ab` 的 `platform_allow` 和 `integration_platforms`：

```yaml
platform_allow:
  - nrf54h20dk/nrf54h20/cpuapp
  - nrf54l15dk/nrf54l10/cpuapp
  - nrf54l15dk/nrf54l15/cpuapp
  - nrf54lm20dk/nrf54lm20a/cpuapp
integration_platforms:
  - nrf54h20dk/nrf54h20/cpuapp
  - nrf54l15dk/nrf54l10/cpuapp
  - nrf54l15dk/nrf54l15/cpuapp
  - nrf54lm20dk/nrf54lm20a/cpuapp
```

## nRF54LM20 支持

### 新增文件

- `boards/nrf54lm20dk_nrf54lm20a_cpuapp.conf`
- `boards/nrf54lm20dk_nrf54lm20a_cpuapp.overlay`
- `sysbuild/mcuboot/boards/nrf54lm20dk_nrf54lm20a_cpuapp.conf`
- `sysbuild/mcuboot/boards/nrf54lm20dk_nrf54lm20a_cpuapp.overlay`
- `sysbuild/nrf54lm20dk_nrf54lm20a_cpuapp_common.dtsi`
- `pm_static_nrf54lm20dk_nrf54lm20a_cpuapp.yml`

### Retained Memory

LM20 的 common DTS 在 SRAM 顶部预留 4KB retained memory：

```dts
cpuapp_sram@2007ec00 {
	compatible = "zephyr,memory-region", "mmio-sram";
	reg = <0x2007ec00 DT_SIZE_K(4)>;
	zephyr,memory-region = "RetainedMem";
	status = "okay";

	retainedmem {
		compatible = "zephyr,retained-ram";
		status = "okay";

		boot_request: boot_request@0 {
			compatible = "zephyr,retention";
			status = "okay";
			reg = <0x0 0x10>;
			prefix = [0B 01];
			checksum = <4>;
		};
	};
};
```

同时通过 `/chosen` 指定：

```dts
chosen {
	nrf,bootloader-request = &boot_request;
};
```

### 配置项

应用侧和 MCUboot 侧都启用：

```conf
CONFIG_RETAINED_MEM=y
CONFIG_RETENTION=y
```

LM20 还显式设置 flash size：

```conf
CONFIG_FLASH_SIZE=2036
```

MCUboot 侧禁用 `fprotect`：

```conf
CONFIG_FPROTECT=n
```

### 静态分区

LM20 使用 `pm_static_nrf54lm20dk_nrf54lm20a_cpuapp.yml` 固定分区布局。核心布局为：

- `mcuboot`: `0x0..0x10800`
- `mcuboot_primary`: `0x11000..0x103800`
- `mcuboot_secondary`: `0x103800..0x1f6000`
- `littlefs_storage`: `0x1f6000..0x1fc000`
- `sram_primary`: `0x20000000..0x2002f000`

其中 primary 和 secondary slot 等长：

```yaml
mcuboot_primary:
  size: 0xf2800
mcuboot_secondary:
  size: 0xf2800
```

应用镜像区域也等长：

```yaml
mcuboot_primary_app:
  size: 0xf2000
mcuboot_secondary_app:
  size: 0xf2000
```

## nRF54L10 支持

### 新增文件

- `boards/nrf54l15dk_nrf54l10_cpuapp.conf`
- `boards/nrf54l15dk_nrf54l10_cpuapp.overlay`
- `sysbuild/mcuboot/boards/nrf54l15dk_nrf54l10_cpuapp.conf`
- `sysbuild/mcuboot/boards/nrf54l15dk_nrf54l10_cpuapp.overlay`
- `sysbuild/nrf54l15dk_nrf54l10_cpuapp_common.dtsi`

没有新增 `pm_static_nrf54l15dk_nrf54l10_cpuapp.yml`，因为动态 Partition Manager 构建已经通过。

### Retained Memory

L10 的 SRAM 范围为：

```text
base = 0x20000000
size = 192 KiB = 0x30000
end  = 0x20030000
```

为避免 16 字节 retained region 太小导致问题，在 SRAM 顶部预留 1KB：

```text
start = 0x20030000 - 0x400 = 0x2002fc00
range = 0x2002fc00..0x2002ffff
```

对应 DTS：

```dts
cpuapp_sram@2002fc00 {
	compatible = "zephyr,memory-region", "mmio-sram";
	reg = <0x2002fc00 DT_SIZE_K(1)>;
	zephyr,memory-region = "RetainedMem";
	status = "okay";

	retainedmem {
		compatible = "zephyr,retained-ram";
		status = "okay";

		boot_request: boot_request@0 {
			compatible = "zephyr,retention";
			status = "okay";
			reg = <0x0 0x10>;
			prefix = [0B 01];
			checksum = <4>;
		};
	};
};
```

`boot_request` 仍只占用 retained memory 起始的 16 字节。

### 配置项

应用侧启用：

```conf
CONFIG_RETAINED_MEM=y
CONFIG_RETENTION=y
```

MCUboot 侧基于 L15 的配置，包含：

```conf
CONFIG_PM_PARTITION_SIZE_MCUBOOT=0x10800
CONFIG_FPROTECT=n
CONFIG_RETAINED_MEM=y
CONFIG_RETENTION=y
```

### 动态分区结果

L10 使用动态 Partition Manager 构建成功。构建后确认：

- `CONFIG_NRF_MCUBOOT_BOOT_REQUEST=y`
- `CONFIG_NRF_MCUBOOT_BOOT_REQUEST_IMPL_RETENTION=y`
- `CONFIG_RETAINED_MEM=y`
- `CONFIG_RETENTION=y`
- `RetainedMem` 区域大小为 1KB

动态 PM 生成的 A/B slot 等长：

```yaml
mcuboot_primary:
  size: 0x73000
mcuboot_secondary:
  size: 0x73000
```

应用镜像区域等长：

```yaml
mcuboot_primary_app:
  size: 0x72800
mcuboot_secondary_app:
  size: 0x72800
```

因此 L10 不需要静态分区文件。

## LM20 与 L10 的差异


| 项目                   | nRF54LM20                                     | nRF54L10                     |
| -------------------- | --------------------------------------------- | ---------------------------- |
| Target               | `nrf54lm20dk/nrf54lm20a/cpuapp`               | `nrf54l15dk/nrf54l10/cpuapp` |
| Retained memory      | 4KB                                           | 1KB                          |
| Retained memory 起始地址 | `0x2007ec00`                                  | `0x2002fc00`                 |
| `boot_request` 大小    | 16 bytes                                      | 16 bytes                     |
| 是否使用静态 PM            | 是                                             | 否                            |
| 静态分区文件               | `pm_static_nrf54lm20dk_nrf54lm20a_cpuapp.yml` | 无                            |
| Flash size 配置        | `CONFIG_FLASH_SIZE=2036`                      | 使用板级默认/动态 PM                 |
| 构建脚本                 | `build_ab_nRF54LM20.bat`                      | `build_ab_nRF54L10.bat`      |


## 总结

LM20 的支持重点是显式释放/使用完整 CPUAPP flash 空间，并通过静态 PM 固定 Direct XIP A/B slot 布局。

L10 的支持重点是补齐应用和 MCUboot 两侧的 board overlay/conf，使 retained memory boot request 生效。L10 默认分区和动态 PM 已能生成满足 A/B Direct XIP 的等长 primary/secondary slot，因此不需要静态分区文件。

以上结论仅在 `NCS v3.2.3` 测试环境下验证通过。如果迁移到其他 NCS 版本，需要重新检查板级 DTS、Partition Manager 输出、MCUboot 配置和构建产物。