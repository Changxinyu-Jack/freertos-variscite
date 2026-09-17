# MCUXpresso SDK 编译指南（DART-MX8MM / Cortex-M4）

> ⚠️ **本文档的目标板是 DART-MX8MM。** 当前实际使用的板子已换成 **Variscite SOM-MX8QM**，
> 其完整流程（换设备树、remoteproc 启动 M4、RPMsg 通信）见
> [`SOM-MX8QM_M4_RPMSG_GUIDE.md`](SOM-MX8QM_M4_RPMSG_GUIDE.md)。
> 两份文档的目录结构、构建配置含义、TCM/DDR 取舍都不同，别混用。

适用：`freertos-variscite-mcuxpresso_sdk_2.9.x-var01`，目标板 DART-MX8MM，构建配置 ddr_debug。

## 1. 最短路径（本次实际跑通的命令）

```bash
cd /home/cxy/work_local/IMX8/freertos-variscite-mcuxpresso_sdk_2.9.x-var01/boards/dart_mx8mm/demo_apps/hello_world/armgcc

export ARMGCC_DIR=/home/cxy/work_local/IMX8/freertos-variscite-mcuxpresso_sdk_2.9.x-var01/gcc-arm-none-eabi-9-2020-q2-update

./build_ddr_debug.sh
```

产物：

```
armgcc/ddr_debug/hello_world.elf   270 KB   含调试符号，给 GDB / J-Link
armgcc/ddr_debug/hello_world.bin    13 KB   裸二进制，给 U-Boot bootaux / remoteproc
```

`text 13588 / data 108 / bss 2124`。

## 2. 唯一的前提：ARMGCC_DIR

这个 SDK 把工具链**放在仓库内部**，不在 `PATH` 上：

```
gcc-arm-none-eabi-9-2020-q2-update/         ← 版本 9.3.1 (9-2020-q2-update)
└── bin/arm-none-eabi-gcc
```

构建系统在 `tools/cmake_toolchain_files/armgcc.cmake:12` 读 `$ENV{ARMGCC_DIR}`，读不到就在第 16 行 Fatal Error 退出：

```cmake
SET(TOOLCHAIN_DIR $ENV{ARMGCC_DIR})
IF(NOT TOOLCHAIN_DIR)
    MESSAGE(FATAL_ERROR "***Please set ARMGCC_DIR in envionment variables***")
ENDIF()
```

**每个新开的终端都要 export 一次。** 想省事可以写进 `~/.bashrc`：

```bash
echo 'export ARMGCC_DIR=/home/cxy/work_local/IMX8/freertos-variscite-mcuxpresso_sdk_2.9.x-var01/gcc-arm-none-eabi-9-2020-q2-update' >> ~/.bashrc
```

## 3. 五种构建配置

脚本名对应 `CMAKE_BUILD_TYPE`，差异只在**链接脚本**和**优化等级**（见 `armgcc/flags.cmake`）：

| 脚本 | CMAKE_BUILD_TYPE | 链接脚本 | 优化 | 代码跑在哪 |
|---|---|---|---|---|
| `build_debug.sh` | `debug` | `..._cm4_ram.ld` | `-O0 -g` | 片上 RAM |
| `build_release.sh` | `release` | `..._cm4_ram.ld` | `-O3` | 片上 RAM |
| `build_ddr_debug.sh` | `ddr_debug` | `..._cm4_ddr_ram.ld` | `-O0 -g` | DDR ← **常用** |
| `build_ddr_release.sh` | `ddr_release` | `..._cm4_ddr_ram.ld` | `-O3` | DDR |
| `build_flash_debug.sh` | `flash_debug` | `..._cm4_flash.ld` | `-O0 -g` | TCM / flash |
| `build_flash_release.sh` | `flash_release` | `..._cm4_flash.ld` | `-O3` | TCM / flash |

选哪个：**和 Linux 一起跑选 `ddr_debug` 或 `ddr_release`**。i.MX8MM 的 DDR 链接脚本把代码放在 `0x7E000000` 起（`MIMX8MM6xxxxx_cm4_ddr_ram.ld:35`），这正是 U-Boot `bootaux` 要加载的地址。`flash_*` 是给裸机 TCM 场景的，i.MX8 Linux 上基本用不到。

每个脚本的动作都一样，四步：

```sh
rm -f CMakeCache.txt Makefile CMakeFiles/     # 清掉上次配置
cmake -DCMAKE_TOOLCHAIN_FILE=... -DCMAKE_BUILD_TYPE=<type> .   # 重新配置
make                                          # 编译
# 完整日志写到 armgcc/build_log.txt
```

## 4. 编译其他例程

流程完全一样，只是换个目录：**进到那个例程自己的 `armgcc/`，跑它自己的 `build_*.sh`。** `ARMGCC_DIR` 是全局的，不用改。

以 `freertos_hello` 为例：

```bash
cd boards/dart_mx8mm/rtos_examples/freertos_hello/armgcc
./build_ddr_debug.sh
# → freertos_hello/armgcc/ddr_debug/freertos_hello.{elf,bin}
```

列出所有可编译例程（DART-MX8MM 共 47 个）：

```bash
find boards/dart_mx8mm -name armgcc -type d | sort
```

分类：

```
demo_apps/               hello_world
driver_examples/         gpio, uart, i2c, ecspi, pwm, gpt, sdma, sema4, wdog, tmu, rdc
cmsis_driver_examples/   uart, i2c, ecspi
rtos_examples/           freertos_hello, _generic, _uart, _i2c, _ecspi, _event, _mutex,
                         _queue, _sem, _swtimer, _tickless
multicore_examples/      rpmsp_lite_str_echo_rtos, rpmsg_lite_pingpong_rtos/linux_remote
```

### ⚠️ 两个坑

**坑 1：必须在例程自己的 `armgcc/` 目录里执行，不能用绝对路径从别处调。**

脚本里到工具链的路径是写死的相对路径，而且**深度随例程嵌套层数变化**：

```
boards/dart_mx8mm/rtos_examples/freertos_event/armgcc        → ../../../../../(5层)
boards/dart_mx8mm/rtos_examples/freertos_ecspi/ecspi_loopback/armgcc → ../../../../../../(6层)
boards/dart_mx8mm/driver_examples/i2c/interrupt_b2b_transfer/slave/armgcc → ../../../../../../../(7层)
```

所以脚本不能拷来拷去，也别想着一份脚本编所有例程。

**坑 2：配置失败会留下"半成品"目录，看起来像编译失败。**

cmake 在配置阶段挂掉时（最常见就是 `ARMGCC_DIR` 没设），目录里会残留 `CMakeCache.txt`，但**没有 Makefile**。这种情况不是代码问题，设好环境变量重跑脚本即可 —— 脚本开头会自己清干净。

判断方法：

```bash
ls armgcc/Makefile        # 存在 = 配置成功过
```

也可以直接看 `CMakeCache.txt` 里有没有编译器条目，只有三个 `CMAKE_BUILD_TYPE` / `CMAKE_TOOLCHAIN_FILE` / `CMAKE_CACHEFILE_DIR` 就是配置没走完。

## 5. 常用操作

```bash
# 完全清理（删掉所有 build_type 子目录、Makefile、CMakeCache）
./clean.sh

# 六种配置全编一遍（调试用，一般不需要）
./build_all.sh

# 只编新的目标，不重新 cmake
make
```

## 6. 可选：VS Code 集成

SDK 自带脚本，会生成 `.vscode/`（含 `tasks.json` / `launch.json` / SVD 文件），支持 F5 调试：

```bash
cd /home/cxy/work_local/IMX8/freertos-variscite-mcuxpresso_sdk_2.9.x-var01

# 单个例程
./var_add_vscode_support.sh -b dart_mx8mm \
    -e boards/dart_mx8mm/demo_apps/hello_world \
    -t ddr \
    -d /opt/SEGGER/JLink_Linux_V754c_x86_64

# 全部例程
./var_add_vscode_support.sh -b dart_mx8mm -e all -t ddr -d <JLink目录>
```

`-d` 指向 SEGGER J-Link 安装目录（`launch.json` 要用 `JLinkGDBServer`）。**当前机器上 `/opt/SEGGER/` 不存在，J-Link 没装**，所以调试这条路暂时走不通；`build` task 本身不依赖 J-Link，可以只用它来编译。

## 7. 关于运行

本次只做了编译，运行方式没有实测。两条路线供参考，具体步骤以官方文档为准：

**A. J-Link 调试器**（SDK 内置方式）
`launch.json` 走 cortex-debug + `JLinkGDBServer`，J-Link 脚本为 `iMX8MM/NXP_iMX8M_Connect_CortexM4.JLinkScript`。需要装 SEGGER J-Link，目前没装。适合单独调 CM4，和 Linux 不太联动。

**B. U-Boot bootaux + Linux remoteproc**（Variscite 方式，和 Linux 共存）
把 `.bin` 放到 SD 卡，U-Boot 里加载到 DDR `0x7E000000` 并 `bootaux`，然后启动 Linux 内核；A53 侧通过 remoteproc / rpmsg 通信。

- 参见 `docs/Getting Started with MCUXpresso SDK for EVK-MIMX8MM.pdf`
- 每个例程目录下有 `readme.txt`，含串口配置和运行步骤
- 多核通信用 `multicore_examples/rpmsg_lite_str_echo_rtos/`，其 `readme.txt` 里有 `/dev/ttyRPMSG30` 的用法
- Variscite Wiki（含 1GB DDR 的地址改造说明）：`https://variwiki.com/index.php?title=MCUXpresso`
- 仓库里的 `var_add_1GB_support.sh` 是为 1GB DDR 的 MX8M-Plus 重映射链接脚本地址用的，**MX8MM 不适用**

## 8. 换板子

工程里一共 13 个板子变体，目录结构一致，换个目录即可：

```
target:   dart_mx8mm   dart_mx8mp   dart_mx8mq
          som_mx8mn    som_mx8mp    som_mx8qm    som_mx8qx
          evkmimx8mm   evkmimx8mn   evkmimx8mp   evkmimx8mq
          mekmimx8qm   mekmimx8qx
```

注意 Cortex-M 核心不同（**MM/MQ/QM/QX 是 M4，MP/MN 是 M7**），设备名和链接脚本前缀随之改变：

| 板子 | 设备 | 核 | 链接脚本实际文件名 |
|---|---|---|---|
| DART-MX8MM | MIMX8MM6 | M4 | `MIMX8MM6xxxxx_cm4_{ram,ddr_ram,flash}.ld` |
| DART-MX8MQ | MIMX8MQ6 | M4 | `MIMX8MQ6xxxJZ_cm4_*.ld` |
| DART-MX8MP / SOM-MX8MP | MIMX8ML8 | M7 | `MIMX8ML8xxxxx_cm7_*.ld` |
| SOM-MX8MN | MIMX8MN6 | M7 | `MIMX8MN6xxxxx_cm7_*.ld` |
| SOM-MX8QX | MIMX8QX6 | M4 | `MIMX8QX6xxxxx_cm4_*.ld` |
| SOM-MX8QM | MIMX8QM6 | M4 ×2 | `MIMX8QM6xxxFF_cm4_core{0,1}_*.ld` |

SOM-MX8QM 是唯一有**双 Cortex-M4**的板子，链接脚本带 `core0`/`core1` 后缀，目录结构也不一样（多一层 `cm4_core0` / `cm4_core1`），要配合 `var_add_vscode_support.sh` 的 `-c cm_c0|cm_c1` 选择核心。

## 9. 排错速查

| 现象 | 原因 | 处理 |
|---|---|---|
| `***Please set ARMGCC_DIR in envionment variables***` | 环境变量没设 | `export ARMGCC_DIR=<SDK根>/gcc-arm-none-eabi-9-2020-q2-update` |
| 目录里只有 `CMakeCache.txt`，没有 Makefile | cmake 配置阶段失败 | 同上，重跑 `build_*.sh` |
| `cmake: command not found` | 没装 cmake | 本机 cmake 3.22.1、make 4.3 已就绪 |
| 找不到 `tools/cmake_toolchain_files/armgcc.cmake` | 没在例程自己的 `armgcc/` 里跑 | `cd` 到该例程的 `armgcc/` |
| `arm-none-eabi-gcc: not found` | 系统 PATH 里没有 | 正常，本 SDK 不依赖 PATH，靠 `ARMGCC_DIR` |

## 附：本次编译的实际输出

```
-- TOOLCHAIN_DIR: /home/cxy/work_local/IMX8/freertos-variscite-mcuxpresso_sdk_2.9.x-var01/gcc-arm-none-eabi-9-2020-q2-update
-- BUILD_TYPE: ddr_debug
-- The C compiler identification is GNU 9.3.1
-- Configuring done / Generating done
[ 94%] Building C object .../system_MIMX8MM6_cm4.c.obj
[100%] Linking C executable ddr_debug/hello_world.elf
[100%] Built target hello_world.elf
```
