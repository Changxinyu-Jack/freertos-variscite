# SOM-MX8QM Cortex-M4 + RPMsg 实操指南

适用：Variscite **VAR-SOM-MX8 (i.MX8QM)** on Symphony-Board LVDS，Linux 内核 6.6.144，
SDK `freertos-variscite-mcuxpresso_sdk_2.9.x-var01`。

目标：在 A53 侧的 Linux 上通过 `remoteproc` 加载 CM4 固件，并用 RPMsg 与 M4 双向通信。

> 本文件记录的是 **2026-09-17 在真实硬件上跑通的完整流程**，每一步都实测过。
> 注意与仓库里的 `BUILD_GUIDE.md` 区分：那份写的是 DART-MX8MM，目标板不同。

**相关文档：**

| 文档 | 内容 |
|---|---|
| `BUILD_GUIDE.md` | DART-MX8MM 的编译流程（**不是这块板**） |
| **本文** | SOM-MX8QM 的上板运行流程：换设备树、启动 M4、RPMsg 通信 |
| [`SOM-MX8QM_M4_FIRMWARE_DEV_GUIDE.md`](SOM-MX8QM_M4_FIRMWARE_DEV_GUIDE.md) | **改固件代码、编译、调试、踩坑记录**（含 CAN 收帧转发的完整示例） |

---

## 0. 总览与常见误解

**内核不用重编，模块不用加，只需要换一个设备树 + 用 TCM 配置编一次 M4 固件。**

| 环节 | 结论 |
|---|---|
| 内核 / 驱动 | 不用动。`imx_rproc` 已是 built-in (`=y`)，rpmsg 协议栈也在 |
| 内核模块 | 不用加。缺的不是模块，是设备树节点 |
| 设备树 | **必须换**成 Variscite 随包附带的 `*-m4.dtb` |
| M4 固件 | 必须用 **TCM/RAM** 配置编，不能用 DDR 配置 |
| 用户态 | `modprobe imx_rpmsg_tty`（不是 `rpmsg_tty`） |

M4 的启动地址由设备树的 `fsl,entry-address` 决定，官方值 `0x34fe0000` 是 **TCML 的系统地址**，
所以固件必须是链接到 TCM 的那个配置。这是整件事里最容易踩的坑。

---

### 0.1 三个容易走岔的认知

### 误解一：「编个 remoteproc 内核模块装上就行了」

**不行。** remoteproc 是「总线 + 驱动」框架：驱动负责实现，**设备必须由设备树提供**。

`imx_rproc.ko` 其实已经编在内核里了（用 `modules.builtin` 可查，见 §8.1）。
它空转的原因是 `/proc/device-tree` 里**没有任何 `fsl,imx8qm-cm4` 节点** ——
驱动没东西可 probe，所以 `/sys/class/remoteproc/` 是空的。

打个比方：显卡驱动装好了，但主板上没插显卡。

### 误解二：「设备树里加一个 M4 节点就行了」

是**三处**改动，缺一不可（手改时的完整内容见附录 A）：

| 改动 | 缺了会怎样 |
|---|---|
| `imx8qm-cm4-0` 节点 | 驱动不 probe，`remoteproc0` 不出现 |
| 6 个 reserved-memory 节点 | probe 报 `unable to acquire memory-region` |
| MU5/MU6 `status = "okay"` | `mbox_request_channel` 失败，probe 直接挂 |

设备树是自洽的：节点里 `mboxes` / `memory-region` 指到谁，谁就得存在且可用。

> 不过本板**不用手改** —— Variscite 已经提供了配好的 `-m4` dtb（见 §2）。

### 误解三：「echo 到 tty 没报错，说明 M4 收到了」

**不说明。** 写 tty 成功只表示内核 buffer 收下了，既不证明送达、也不证明有回显。
真正的证据链见 §5。

---

## 1. 编译 M4 程序

### 1.1 前提：ARMGCC_DIR

工具链在仓库内部，不在 `PATH` 上。**每个新终端都要 export 一次**：

```bash
export ARMGCC_DIR=/home/cxy/work_local/IMX8/freertos-variscite-mcuxpresso_sdk_2.9.x-var01/gcc-arm-none-eabi-9-2020-q2-update
```

`tools/cmake_toolchain_files/armgcc.cmake:16` 读不到就直接 Fatal Error。

### 1.2 进入例程目录（注意多一层 `cm4_core0`）

```bash
cd /home/cxy/work_local/IMX8/freertos-variscite-mcuxpresso_sdk_2.9.x-var01/boards/som_mx8qm/multicore_examples/rpmsg_lite_str_echo_rtos/cm4_core0/armgcc
```

SOM-MX8QM 是全树**唯一有双 Cortex-M4** 的板子，每个例程都在 `cm4_core0/` 或 `cm4_core1/` 下面。

### 1.3 六种配置

| 脚本 | 链接脚本 | 代码位置 | 能否配合 `-m4` dtb |
|---|---|---|---|
| `build_debug.sh` | `*_ram.ld` | **TCM** | ✅ **用这个** |
| `build_release.sh` | `*_ram.ld` | TCM | ✅ |
| `build_ddr_debug.sh` | `*_ddr_ram.ld` | DDR `0x88000000` | ❌ **不能用** |
| `build_ddr_release.sh` | `*_ddr_ram.ld` | DDR | ❌ |
| `build_flash_debug.sh` | `*_flash.ld` | flash | ❌ |
| `build_flash_release.sh` | `*_flash.ld` | flash | ❌ |

**为什么 DDR 版不行**：`-m4` dtb 里 `fsl,entry-address = <0x34fe0000>`，驱动
（`drivers/remoteproc/imx_rproc.c:1080` 读取，`:427` 传给 SCU）会让 M4 从 TCML 开始执行；
而 DDR 版固件的代码在 `0x88000000`，M4 会从空内存启动。

### 1.4 编译并确认

```bash
./build_debug.sh
```

产物在 `debug/`：

```
rpmsg_lite_str_echo_rtos_imxcm4_m40.elf   524672 B   给 remoteproc（它直接吃 ELF）
rpmsg_lite_str_echo_rtos_m40.bin           36512 B   裸二进制，备用
```

实测布局（`arm-none-eabi-readelf -l`）：

| 段 | M4 视角 | 系统视角 | 大小 |
|---|---|---|---|
| entry point | `0x1ffe0b5d` | `0x34fe0b5d` | — |
| `.interrupts` | `0x1ffe0000` | `0x34fe0000` | 0xa00 |
| `.resource_table` + `.text` | `0x1ffe0a00` | `0x34fe0a00` | 0x8438 |
| `.data` + `.bss` | `0x20000000` | `0x35000000` | 0x5d28 / 0x68 |

全部在 TCM 内（TCML 128 KB / TCMU 128 KB，用量宽裕），不占 DDR。

> `cm4_core1/` 编出来的是 `..._m41.elf`。core1 流程相同，本文档未在硬件上验证。

### 1.5 编其他例程 / 其他板子

换个例程就是换个目录，流程完全一样：

```bash
# 列出 som_mx8qm 所有可编译例程（共 181 个）
find boards/som_mx8qm -name armgcc -type d | sort
```

```
demo_apps/               hello_world, power_mode_switch
driver_examples/         gpio, lpuart, lpi2c, lpspi, canfd, edma, enet ... （97 个）
cmsis_driver_examples/   enet, lpi2c, lpspi, lpuart
rtos_examples/           freertos_hello, _generic, _lpuart, _tickless ... （26 个）
multicore_examples/      rpmsg_lite_str_echo_rtos, rpmsg_lite_pingpong_rtos
lwip_examples/           24 个
mmcau_examples/
```

**脚本不能跨目录复用** —— 里面指向工具链的相对路径是写死的，层数还随例程嵌套深度变化。
必须 `cd` 到该例程自己的 `armgcc/` 再跑。

树里共 13 个板子变体：

```
dart_mx8mm  dart_mx8mp  dart_mx8mq
som_mx8mn   som_mx8mp   som_mx8qm   som_mx8qx
evkmimx8mm  evkmimx8mn  evkmimx8mp  evkmimx8mq
mekmimx8qm  mekmimx8qx
```

核不同：**MM / MQ / QM / QX 是 Cortex-M4，MP / MN 是 Cortex-M7**。
QM 在这棵树里有两个目录：`som_mx8qm`（Variscite SOM，本板）和 `mekmimx8qm`（NXP MEK）。

**没有"默认板子"** —— 选板子就是进哪个目录。唯一存在的默认值在
`var_add_vscode_support.sh:10-11`（`CM_ID=cm_c0` / `CM4_CORE_DIR=cm4_core0`，
只影响生成 `.vscode/`，不影响编译）。

---

## 2. 更换设备树

### 2.1 为什么要换

板子原配的普通 dtb（`imx8qm-var-som-symphony-lvds.dtb`）**完全没有任何 CM4 节点**：
没有 `fsl,imx8qm-cm4`、没有 M4 的 reserved-memory，MU5/MU6 也是 `disabled`。
驱动 `imx_rproc` 虽然在内核里，但没有设备可 probe，所以 `/sys/class/remoteproc/` 是空的。

Variscite 随 BSP 附带了**已经配好的 `-m4` 变体**，不用自己改设备树。

### 2.2 板子 `/boot` 下已有的 `-m4` dtb

```
imx8qm-var-som-symphony-lvds-m4.dtb        ← 本板用这个
imx8qm-var-som-symphony-hdmi-m4.dtb
imx8qm-var-som-symphony-1.x-lvds-m4.dtb
imx8qm-var-som-symphony-1.x-hdmi-m4.dtb
imx8qm-var-som-symphony-1.x-dp-m4.dtb
imx8qm-var-som-symphony-dp-m4.dtb
imx8qm-var-spear-sp8customboard*-m4.dtb
```

### 2.3 `-m4` 版和普通版的差异

**打开**（给 M4 通信）：

| 节点 | 变化 |
|---|---|
| `mailbox@5d200000` (MU5) / `mailbox@5d210000` (MU6) | `disabled → okay` |

**关闭**（把外设**交给 M4**，Linux 侧不再拥有）：

| 节点 | 别名 | 说明 |
|---|---|---|
| `serial@5a080000` | serial2 (LPUART2) | |
| `serial@5a0a0000` | serial4 (LPUART4) | **M4 自己的 console**，Linux 必须放掉 |
| `i2c@5a800000` | i2c0 | |
| `can@5a8d0000` | can0 | |
| `intmux@37400000` / `intmux@3b400000` | — | M4 的 INTMUX |

**Linux 自己的 console 不受影响** —— 两个版本的 `stdout-path` 都是
`/bus@5a000000/serial@5a060000`（LPUART0）。关掉的是 LPUART2/4。

**这也意味着切过去之后 Linux 里就找不到 LPUART2 / LPUART4 / i2c0 / can0 了，
这是预期行为，不是故障。**

新增的 M4 相关节点（地址与 SDK `cm4_core0/board.h:107-116` 严格对应）：

```
imx8qm-cm4-0 / imx8m-cm4-1       compatible = "fsl,imx8qm-cm4"
                                 fsl,entry-address = <0x34fe0000>
                                 fsl,resource-id   = <278>   (IMX_SC_R_M4_0_PID0)
vdev0vring0@90000000   vdev0vring1@90008000      ← SRTM vring (VDEV0)
vdev1vring0@90010000   vdev1vring1@90018000      ← USER vring (VDEV1)
rsc-table@900ff000                                ← RESOURCE_TABLE_OFFSET = 0xFF000
vdevbuffer@90400000                               ← shared-dma-pool
core1 另有一套 0x90100000 / 0x90110000
```

> 节点名有讲究：驱动 `imx_rproc.c:814` 靠节点名 `rsc-table` 定位资源表，
> `imx_rproc.c:604` 靠名字前缀 `vdev` 跳过 vring。手改设备树时名字写错会 probe 失败。

### 2.4 操作步骤

**修改前先备份，这是唯一的保险** —— 这棵内核树没有 git commit，
`.dts.orig` 之类的手工备份就是回滚的全部依据。

板子关机，SD 卡插到 PC 上（只有一个 ext4 分区，label `rootfs`），然后：

```bash
# 1) 备份当前 dtb
sudo cp /media/cxy/rootfs/boot/imx8qm-var-som-symphony-lvds.dtb \
        /media/cxy/rootfs/boot/imx8qm-var-som-symphony-lvds.dtb.nom4.bak

# 2) 用 -m4 版覆盖原文件名
sudo cp /media/cxy/rootfs/boot/imx8qm-var-som-symphony-lvds-m4.dtb \
        /media/cxy/rootfs/boot/imx8qm-var-som-symphony-lvds.dtb

# 3) 校验
md5sum /media/cxy/rootfs/boot/imx8qm-var-som-symphony-lvds*.dtb*

sudo sync
udisksctl unmount -b /dev/sdb1     # 安全卸载后拔卡
```

预期 md5（本次实测值）：

```
b3ad414b466e839a90b96ed94095bf5a   imx8qm-var-som-symphony-lvds.dtb          ← 已换
bc7cb3f4cb41fd9c642ea6f4ec3b7605   imx8qm-var-som-symphony-lvds.dtb.nom4.bak ← 备份
b3ad414b466e839a90b96ed94095bf5a   imx8qm-var-som-symphony-lvds-m4.dtb       ← 原件未动
```

> **覆盖文件名而不是改 U-Boot 的 `fdt_file`**，是因为不确定 bootcmd 是否走这个变量；
> 覆盖文件名对两种 boot 流程都生效。SD 卡上只有一个分区、`/boot` 下也没有
> `boot.scr` / `uEnv.txt`，所以没法从卡上直接确认 boot 流程。

### 2.5 回滚

```bash
sudo cp /media/cxy/rootfs/boot/imx8qm-var-som-symphony-lvds.dtb.nom4.bak \
        /media/cxy/rootfs/boot/imx8qm-var-som-symphony-lvds.dtb
sudo sync
```

或者在 U-Boot 里（若 bootcmd 走变量）：

```
=> setenv fdt_file imx8qm-var-som-symphony-lvds-m4.dtb
=> saveenv
```

### 2.6 验证设备树生效

```bash
ls /proc/device-tree | grep cm4
# 期望: clock-cm40-ipg  clock-cm41-ipg  imx8m-cm4-1  imx8qm-cm4-0

ls /sys/class/remoteproc/
# 期望: remoteproc0  remoteproc1     ← 两个核都出来了
```

---

## 3. 启动 M4

### 3.1 认名字，别按编号猜

```bash
cat /sys/class/remoteproc/remoteproc0/name
cat /sys/class/remoteproc/remoteproc1/name
```

期望看到 `imx8qm-cm4-0`（core0）和 `imx8m-cm4-1`（core1，`imx8m` 是 NXP 的拼写笔误，
不影响功能）。下面的 `remoteproc0` 请换成你实际上认出来的 core0 那个。

### 3.2 放固件并启动

```bash
cp rpmsg_lite_str_echo_rtos_imxcm4_m40.elf /lib/firmware/

echo rpmsg_lite_str_echo_rtos_imxcm4_m40.elf > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state
cat /sys/class/remoteproc/remoteproc0/state       # 期望: running
```

### 3.3 期望的 dmesg

```
remoteproc remoteproc0: powering up imx-rproc
remoteproc remoteproc0: Booting fw image rpmsg_lite_str_echo_rtos_imxcm4_m40.elf, size 524672
rproc-virtio rproc-virtio.0.auto: assigned reserved memory node vdevbuffer@90400000
virtio_rpmsg_bus virtio0: rpmsg host is online
rproc-virtio rproc-virtio.0.auto: registered virtio0 (type 7)
rproc-virtio rproc-virtio.1.auto: assigned reserved memory node vdevbuffer@90400000
virtio_rpmsg_bus virtio1: rpmsg host is online
virtio_rpmsg_bus virtio1: creating channel rpmsg-virtual-tty-channel addr 0x1e   ← 名字服务握手成功
rproc-virtio rproc-virtio.1.auto: registered virtio1 (type 7)
remoteproc remoteproc0: remote processor imx-rproc is now up
```

`addr 0x1e` = 30，和 SDK readme 里的 `/dev/ttyRPMSG30` 对得上。

### 3.4 停止 / 重启

```bash
echo stop  > /sys/class/remoteproc/remoteproc0/state    # → offline，/dev/ttyRPMSG* 消失
echo start > /sys/class/remoteproc/remoteproc0/state    # → running，设备重新出现
```

### 3.5 起不来怎么逐步排查

按顺序查，每一关定位到不同的层：

**第 1 关 —— `/sys/class/remoteproc/` 是不是空的**

```bash
ls /sys/class/remoteproc/
ls /proc/device-tree | grep cm4
cat /proc/device-tree/model
```

空 → 设备树没生效，跟 M4 固件无关。先确认 `imx8qm-cm4-0` 节点在不在，
再确认板子加载的是哪份 dtb。

**第 2 关 —— `state` 是 `offline` 还是 `detached`**

```bash
cat /sys/class/remoteproc/remoteproc0/state
```

- `offline` → 正常，可以 `start`
- `detached` → SCFW 没把 M4 划给 Linux 分区，**设备树改得再对也没用**，
  要去动 SCFW 的资源分区配置。本板实测是 `offline`，不会走到这里。

**第 3 关 —— `start` 报不报错**

```bash
dmesg | tail -30
```

常见错：

| 报错 | 原因 |
|---|---|
| `unable to acquire memory-region` | reserved-memory 节点名或 phandle 引用不对 |
| `failed to request mbox` | MU 节点还是 `disabled` |
| `Failed to enable remote core!` | SCU 拒绝了启动请求，多半是资源归属问题 |

**第 4 关 —— `running` 了但用户态没设备**

```bash
dmesg | grep -i "creating channel"
```

- 没有 → M4 固件没真正跑起来，检查是不是又用了 DDR 版（§1.3）
- 有 → M4 侧已经好了，是用户态驱动的问题，去 §4

---

## 4. 用户态通信（RPMsg tty）

### 4.1 先分清两种"串口"

这两个是完全不同的东西，**没接物理串口不影响前者**：

| | 走什么 | 要不要物理接线 |
|---|---|---|
| **和 M4 通信**（收发数据） | RPMsg tty `/dev/ttyRPMSG*` | **不用**，终端里就行 |
| **看 M4 自己的 printf**（banner、`Get Message From Master Side`） | LPUART4 | 要，得把板子 LPUART4 引脚接出来 |

没有物理串口时，你只是看不到 M4 那侧的日志；通信本身是通的，
而且 M4 会把收到的原样回显 —— 回显就是最好的证据（§5）。

### 4.2 加载正确的驱动 —— 这里有个坑

| 模块 | 文件 | 匹配的 channel 名 | 对本例 |
|---|---|---|---|
| `rpmsg_tty` | `drivers/tty/rpmsg_tty.c` | `"rpmsg-tty"` | ❌ 不匹配 |
| `imx_rpmsg_tty` | `drivers/rpmsg/imx_rpmsg_tty.c` | `"rpmsg-virtual-tty-channel"` | ✅ **用这个** |

M4 在 `main_remote.c:30` announce 的名字是 `RPMSG_LITE_NS_ANNOUNCE_STRING =
"rpmsg-virtual-tty-channel"`，所以只有 NXP 那个驱动会认领。

```bash
rmmod rpmsg_tty 2>/dev/null        # 装错的那个，没绑上任何东西，卸掉干净
modprobe imx_rpmsg_tty
```

### 4.3 设备节点

```bash
ls -l /dev/ttyRPMSG*
# -rw-r--r-- 1 root root 17 ... /dev/ttyRPMSG30
```

名字来自 `imx_rpmsg_tty.c:151` 的 `kasprintf("ttyRPMSG%d", rpdev->dst)`，
**以 `ls` 实际为准**，不要硬编码 30。

### 4.4 收发

```bash
# 终端 A
cat /dev/ttyRPMSG30

# 终端 B
echo "hello from linux" > /dev/ttyRPMSG30
# 终端 A 立刻回显出同样的字符串
```

单终端测法：

```bash
( cat /dev/ttyRPMSG30 & sleep 0.5; echo "hello"; sleep 1; kill %1 )
```

输出粘在一起的话：

```bash
stty -F /dev/ttyRPMSG30 raw -echo
```

### 4.5 开机自动加载（可选）

```bash
echo imx_rpmsg_tty > /etc/modules-load.d/rpmsg.conf
```

---

## 5. 怎么证明数据真的经过 M4

写 tty 不报错**什么都不证明** —— 写入成功只说明 buffer 收下了。真正的证据是**回显走的是接收路径**。

### 5.1 驱动自带的自环自检（已通过）

`imx_rpmsg_tty_probe()` 在绑定成功的瞬间会主动发一条固定字符串
（`drivers/rpmsg/imx_rpmsg_tty.c:18` `#define MSG "hello world!"`，`:49` `rpmsg_send(...)`）。

所以 `modprobe` 之后 dmesg 里应出现：

```
imx_rpmsg_tty virtio1.rpmsg-virtual-tty-channel.-1.30: new channel: 0x400 -> 0x1e!
Install rpmsg tty driver!
rpmsg_tty_cb 68 65 6c 6c 6f 20 77 6f 72 6c 64 21    hello world!
```

那串十六进制就是 `hello world!`。三点合起来构成证明：

1. 驱动绑定**时自动发出**这条消息 —— 你从没在终端里输入过它
2. `rpmsg_tty_cb` 是**接收**回调（内部调 `tty_flip_buffer_push()`）
3. 时间戳：发出和收回相隔约 9 ms

唯一的解释是 M4 收到后原样回显。SDK readme 的对应描述是 M4 侧打印
`Get Messgae From Master Side : "hello world!" [len : 12]`。

### 5.2 可控复现（不改代码）

```bash
echo stop  > /sys/class/remoteproc/remoteproc0/state
ls -l /dev/ttyRPMSG*        # 设备消失 —— 证明它依赖 M4 的 virtio channel
echo start > /sys/class/remoteproc/remoteproc0/state
sleep 2
dmesg | tail -8             # 又出现一行 rpmsg_tty_cb ... hello world!
```

全程不敲任何输入，往返自己发生了一次。

> 顺带回答一个常见疑问：「M4 没启动时能不能往 tty 写？」
> **不能 —— 那时 `/dev/ttyRPMSG30` 根本不存在。** 设备节点的存在本身就依赖
> M4 的 virtio channel：M4 起来 → 名字服务握手 → channel 创建 → 驱动 probe → 设备出现。
> 前面那次 `echo > /dev/ttyRPMSG30` 之所以"没报错"，是因为那时 M4 已经在跑了。

### 5.3 打标签法（决定性，需要改固件）

改 `cm4_core0/main_remote.c:110` 附近，让 M4 回显时加前缀：

```c
        /* Get tx buffer from RPMsg */
        tx_buf = rpmsg_lite_alloc_tx_buffer(my_rpmsg, &size, RL_BLOCK);
        assert(tx_buf);
        /* 加标记，用于验证数据确实经过 M4 */
        memcpy(tx_buf, "M4:", 3);
        memcpy((char *)tx_buf + 3, app_buf, len);
        result = rpmsg_lite_send_nocopy(my_rpmsg, my_ept, remote_addr, tx_buf, len + 3);
```

重新 `./build_debug.sh`、换固件、`echo stop` + `echo start`。然后：

```bash
echo abc > /dev/ttyRPMSG30
cat /dev/ttyRPMSG30        # 期望 M4:abc
```

Linux 侧没有任何东西会加 `M4:` 前缀，收到就说明数据真的绕了 M4 一圈。

---

## 6. 排错速查

| 现象 | 原因 | 处理 |
|---|---|---|
| `***Please set ARMGCC_DIR in envionment variables***` | 环境变量没设 | 见 §1.1 |
| 例程目录里只有 `CMakeCache.txt` 没有 `Makefile` | cmake 配置阶段失败 | 设好 `ARMGCC_DIR` 重跑脚本（脚本会自行清理） |
| `/sys/class/remoteproc/` 空 | dtb 里没有 CM4 节点 | 换 `-m4` dtb（§2）；`ls /proc/device-tree \| grep cm4` 自查 |
| `state` 是 `detached` 不是 `offline` | SCFW 没把 M4 划给 Linux 分区 | 本板实测**不会**发生；若遇到需改 SCFW 资源分区 |
| `state` 变不成 `running` | 固件配置不对 | 用 **TCM 版**（`build_debug.sh`），不是 DDR 版 |
| `start` 成功但 M4 没反应 | 同上 | 同上 |
| `/dev/ttyRPMSG*` 不存在 | 装错驱动 | `modprobe imx_rpmsg_tty`，不是 `rpmsg_tty`（§4.2） |
| `modprobe` 报 `Module not found` | 板子 rootfs 里没有该 `.ko` | `find /lib/modules/$(uname -r) -name "*rpmsg*"` |
| 输出粘在一起 | tty 不是 raw 模式 | `stty -F /dev/ttyRPMSG30 raw -echo` |
| Linux 看不到 LPUART4 / LPUART2 / i2c0 / can0 | `-m4` dtb 把这些交给 M4 了 | 预期行为，不是故障（§2.3） |
| 看不到 M4 的 printf 日志 | 那些走 LPUART4，已被 Linux 让出 | 需要物理接 LPUART4 引脚，115200 8N1 |

---

## 7. 附录 A：手改设备树（备选路线，未在硬件验证）

本板最终走的是 §2 的官方 `-m4` dtb。这条路线是当时**先探索出来的**，
文件现在还留在内核树里，记在这里以便需要时启用。

### 7.1 为什么最后没用它

官方 `-m4` dtb 除了加 M4 节点，**还会把 LPUART2 / LPUART4 / i2c0 / can0 / INTMUX
从 Linux 手里拿走交给 M4**。手改的版本只加不减，所以：

| | 官方 `-m4` | 手改版 |
|---|---|---|
| M4 rpmsg | ✅ 实测通过 | ❓ 未验证 |
| Linux 保留 LPUART2/4、i2c0、can0 | ❌ 失去 | ✅ 保留 |
| 是否 Variscite 测试过的组合 | ✅ | ❌ |

如果哪天官方 `-m4` dtb 出问题（比如内核版本演进导致不兼容），手改版是另一条路 ——
它只做加法，**不碰任何现有节点**（唯一例外是打开 MU5/MU6）。

### 7.2 现存文件

```
/home/cxy/work_local/IMX8/linux-fslc-6.6-2.2.x-imx/arch/arm64/boot/dts/freescale/
├── imx8qm-var-som-symphony-lvds.dts.orig   备份原文件    md5 38be69729d056d458b7d9fe09e27b3f0
├── imx8qm-var-som-symphony-lvds.dts        已改         212688 B
└── imx8qm-var-som-symphony-lvds.dtb        编好的 dtb    md5 8bc138f1cd8cfb93aed5210acfab1499
```

回滚：

```bash
cd /home/cxy/work_local/IMX8/linux-fslc-6.6-2.2.x-imx/arch/arm64/boot/dts/freescale
cp imx8qm-var-som-symphony-lvds.dts.orig imx8qm-var-som-symphony-lvds.dts
rm -f imx8qm-var-som-symphony-lvds.dtb
```

### 7.3 这份 `.dts` 的特殊性

它是**反编译产物**，不是手写源码：

- 7260 行、211 KB（对比同类手写源码 `imx8mm-var-som.dtsi` 只有 13 KB）
- phandle 是**数字**（`<0x1f9>`），节点没有 label
- 自带 `__symbols__` 节点
- 没有 `#include`，自包含 —— 所以**可以用 dtc 单独编译，不需要编内核**

### 7.4 三处改动

**改前须知**：文件里现有最大 phandle 是 `0x23b`，所以新节点从 `0x23c` 开始编号不会撞。

**改动 1 —— 打开 MU5 / MU6**（`mailbox@5d200000` 约 3914 行，`mailbox@5d210000` 约 3924 行）

```dts
			power-domains = <0x18 0xda>;      ← MU5，MU6 是 0xdb
			status = "okay";                  ← 原来是 "disabled"
```

> 文件里 `disabled` 的节点很多，**认地址改，别按顺序数**。

**改动 2 —— 在 `reserved-memory` 节点里追加 6 个**（放在 `linux,cma` 块之后、该节点闭合 `};` 之前）

```dts
		m4_0_vdevbuffer: vdevbuffer@90400000 {
			compatible = "shared-dma-pool";
			reg = <0x00 0x90400000 0x00 0x100000>;
			no-map;
			phandle = <0x23c>;
		};

		m4_0_vdev0vring0: vdev0vring0@90000000 {
			reg = <0x00 0x90000000 0x00 0x8000>;
			no-map;
			phandle = <0x23d>;
		};

		m4_0_vdev0vring1: vdev0vring1@90008000 {
			reg = <0x00 0x90008000 0x00 0x8000>;
			no-map;
			phandle = <0x23e>;
		};

		m4_0_vdev1vring0: vdev1vring0@90010000 {
			reg = <0x00 0x90010000 0x00 0x8000>;
			no-map;
			phandle = <0x23f>;
		};

		m4_0_vdev1vring1: vdev1vring1@90018000 {
			reg = <0x00 0x90018000 0x00 0x8000>;
			no-map;
			phandle = <0x240>;
		};

		m4_0_rsc_table: rsc-table@900ff000 {
			reg = <0x00 0x900ff000 0x00 0x1000>;
			no-map;
			phandle = <0x241>;
		};
```

**改动 3 —— 加 CM4 节点**（放在根节点下、`__symbols__ {` 之前）

```dts
	imx8qm_cm40: imx8qm-cm4-0 {
		compatible = "fsl,imx8qm-cm4";
		rsc-da = <0x90000000>;
		mbox-names = "tx", "rx", "rxdb";
		mboxes = <0x1f9 0 1
			  0x1f9 1 1
			  0x1f9 3 1>;
		memory-region = <0x23c 0x23d 0x23e 0x23f 0x240 0x241>;
		fsl,resource-id = <278>;
		fsl,entry-address = <0x34fe0000>;
		status = "okay";
		power-domains = <0x18 278>, <0x18 297>;
	};
```

**数字对照表**：

| 数字 | 含义 | 出处 |
|---|---|---|
| `0x18` | `pd`（power-controller） | `power-domains = <0x18 ...>`，文件 357 行 |
| `0x1f9` / `0x1fa` | MU5 / MU6 | 两节点的 `phandle` |
| `278` | `IMX_SC_R_M4_0_PID0` | `include/dt-bindings/firmware/imx/rsrc.h:294` |
| `297` | `IMX_SC_R_M4_0_MU_1A` | 同文件 `:313` |
| `0x23c`–`0x241` | 本次新分配的 reserved-memory phandle | 现有最大值 `0x23b` + 1 |

### 7.5 编译（不用编内核）

```bash
cd /home/cxy/work_local/IMX8/linux-fslc-6.6-2.2.x-imx
./scripts/dtc/dtc -I dts -O dtb \
  -o arch/arm64/boot/dts/freescale/imx8qm-var-som-symphony-lvds.dtb \
  arch/arm64/boot/dts/freescale/imx8qm-var-som-symphony-lvds.dts
```

只有 warning，无 error。产物体积比原来大 ~840 字节。

> ⚠️ **不要用内核的 `make dtbs`** —— 这棵树的顶层 `.config` 是坏的（§8.1），
> 会触发重新配置。直接调 dtc 最简单可靠。

### 7.6 回归校验方法

改完确认"只有预期改动、没误伤"的做法：把 `.orig` 也编一遍，两边都反编译，然后 diff。

```bash
D=arch/arm64/boot/dts/freescale
./scripts/dtc/dtc -I dts -O dtb -o /tmp/orig.dtb $D/imx8qm-var-som-symphony-lvds.dts.orig
./scripts/dtc/dtc -I dts -O dtb -o /tmp/new.dtb  $D/imx8qm-var-som-symphony-lvds.dts
./scripts/dtc/dtc -I dtb -O dts -o /tmp/orig.dts /tmp/orig.dtb
./scripts/dtc/dtc -I dtb -O dts -o /tmp/new.dts  /tmp/new.dtb
diff /tmp/orig.dts /tmp/new.dts
```

本次结果：**2 行修改 + 纯新增，其余 7000 多行未动，phandle 也没有整体重编号**。

```
3914c3914    status = "disabled"  →  "okay"     ← MU5
3924c3924    status = "disabled"  →  "okay"     ← MU6
6586a6587    + 6 个 reserved-memory 节点
6679a6717    + imx8qm-cm4-0 节点
```

### 7.7 与官方 `-m4` dtb 的对照

反编译官方 `-m4` dtb 可以看到，它新增的 M4 部分和上面**完全等价**
（同样的 `entry-address = 0x34fe0000`、`resource-id = 278`、
`power-domains = <&pd 278>, <&pd 297>`、同一组 vring 地址），
差别只在于它还额外关闭了那 6 个给 M4 的外设节点。

这也**反证了 TCM 固件才是对的** —— 官方 dtb 的 `entry-address` 就是 TCML 系统地址。

---

## 8. 附录 B：本轮排查得到的关键事实

### 8.1 内核侧到底有没有驱动 —— 别读 `.config`

`linux-fslc-6.6-2.2.x-imx` 树里的顶层 `.config` **和实际运行的内核不符**
（缺 `ARCH_MXC`、`SERIAL_IMX`、`IMX_SCU`、`IMX_MBOX`、`IMX_REMOTEPROC`，
用它能编出在 i.MX8QM 上根本起不来的内核 —— 连串口驱动都没有）。
而且它还比构建产物新：`imx_rproc.o` 编译于 8/7 14:13，`.config` 是 16:34，
说明构建完之后被覆盖过。

可靠的办法是看已部署 rootfs 里的 `modules.builtin`：

```bash
grep -iE "rproc|rpmsg" rootfs/lib/modules/6.6.144/modules.builtin
```

实测包含（即 `=y`，编进内核了）：

```
kernel/drivers/remoteproc/imx_rproc.ko
kernel/drivers/rpmsg/rpmsg_core.ko
kernel/drivers/rpmsg/rpmsg_ns.ko
kernel/drivers/rpmsg/virtio_rpmsg_bus.ko
kernel/drivers/rpmsg/imx_rpmsg.ko
```

**结论：要判断"驱动在不在"，`modules.builtin` 比 `.config` 可信。
要重编内核，必须先把配置重新生成，不能直接用树里那份。**

### 8.2 驱动怎么处理 i.MX8QM

`drivers/remoteproc/imx_rproc.c`：

- `:339` `imx_rproc_cfg_imx8qm`，`.method = IMX_RPROC_SCU_API`
- `:1080` 读设备树 `fsl,entry-address` 存进 `priv->entry`
- `:427` start 时 `imx_sc_pm_cpu_start(ipc, rsrc_id, true, priv->entry)` —— 由 SCU 启动 M4
- `:1077` `imx_sc_rm_is_resource_owned()` 决定两条路：
  - 归 A 核 → 读 `entry-address`，`state = offline`，Linux 可 load/start
  - 不归 A 核 → `RPROC_DETACHED`，Linux 只能 IPC，不能加载

  **本板实测走的是第一条**，所以整条路径畅通。

地址翻译表 `imx_rproc_att_imx8qm`：

| 区域 | M4 视角 | 系统视角 |
|---|---|---|
| TCML | `0x1FFE0000` | `0x34FE0000` (core0) / `0x38FE0000` (core1) |
| TCMU | `0x20000000` | `0x35000000` (core0) / `0x39000000` (core1) |
| DDR | `0x80000000` | `0x80000000`（identity，长度 0x60000000） |

### 8.3 本次实测的环境

```
板子    : Variscite VAR-SOM-MX8 i.MX8QM on Symphony-Board LVDS
内核    : 6.6.144
dtb     : imx8qm-var-som-symphony-lvds.dtb (= -m4 内容, md5 b3ad414b...)
固件    : rpmsg_lite_str_echo_rtos_imxcm4_m40.elf (TCM, build_debug.sh)
rproc   : remoteproc0 = imx8qm-cm4-0, state running
tty     : /dev/ttyRPMSG30
```

### 8.4 尚未验证的部分

- core1 的完整流程（`cm4_core1/`，`-m41` 固件，readme 说对应 `/dev/ttyRPMSG31`）
- 附录 A 的手改 dtb 路线（编出来了，但**从没刷到板子上跑过**）
- `imx_rpmsg_tty` 的开机自动加载（§4.5 的做法未实测）
- 断电重启后的状态复现（本次都是运行中操作）
- `ddr_debug` 等其他配置能否在改 `fsl,entry-address` 后配合使用
