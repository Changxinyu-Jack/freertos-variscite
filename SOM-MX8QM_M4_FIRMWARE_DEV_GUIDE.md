# SOM-MX8QM M4 固件：开发、编译、调试手册

配套文档：[`SOM-MX8QM_M4_RPMSG_GUIDE.md`](SOM-MX8QM_M4_RPMSG_GUIDE.md)（上板运行流程）。

本文讲的是**写固件、编固件、以及在"看不见 M4 内部"的情况下怎么定位问题**。
所有内容都来自 2026-09-17 在真实硬件上把「CAN 收帧 → RPMsg 转发给 Linux」跑通的完整过程。

---

## 1. 架构与数据通路

动手前先建立整体图景。**关键是先把两条链路分开**：

| | M4 ↔ SCU | M4 ↔ A核(Linux) |
|---|---|---|
| 硬件 | MU（专用通道，如 MU_7） | MU（另一条，core0 用 MU5） |
| 协议 | SCFW RPC（`sc_*` API） | RPMsg / virtio |
| 用途 | **控制面**：上电、时钟、pad、查资源归属 | **数据面**：业务数据 |
| 谁响应 | SCU | Linux |

固件里 `BOARD_InitRpc()` 建的是**控制面**，`rpmsg_lite_remote_init()` 建的是**数据面**。
两条都跑在 MU 上，但协议和对象完全不同 —— 很多弯路都源于把两者混在一起看。

### 1.1 谁在管这颗 SoC

i.MX8QM 不是"谁都能碰外设"的 SoC。硬件上每个外设都是一个带 ID 的**资源**
（`sc_rsrc_t`，如 `SC_R_CAN_0` = 105），由 SCU 上的 SCFW 统一管理。这带来几条必须早知道的推论：

| 事实 | 影响 |
|---|---|
| **外设受 SCU 管辖** | 写外设前要先 `sc_pm_set_resource_power_mode()` —— 那是**向 SCU 申请**，不是直接写寄存器 |
| **M4 自己也是资源** | `SC_R_M4_0_PID0` = 278。所以 `imx_rproc.c:1077` 能问"我管不管这个 M4"，决定 `offline`（能加载）还是 `detached`（只能 IPC） |
| **调试输出要外接串口线** | `PRINTF` 走 LPUART4，**配置是正确的**（`BOARD_InitDebugConsole()` 已上电 `SC_R_UART_4`、开时钟 24 MHz、设 115200、引脚 `SC_P_M40_GPIO0_00/01` mux 2，与设备树 `lpuart4grp` 一致）。**2026-09-17 已实测接上并看到输出** —— 但开发时手边不一定有线，所以要按 §5.1 设计观测手段 |
| **remoteproc 检测不到 M4 崩溃** | 没有 watchdog。M4 挂死在死循环里，Linux 侧 `state` 依然显示 `running` |
| **这是 FreeRTOS 工程** | 中断里调 `...FromISR()` 有优先级约束（见 §6 案例 5） |

### 1.2 启动链路

```
上电
 │
 │ ① BootROM（芯片内固化，改不了）
 │    读启动介质固定偏移 0x8000(第32扇区)，找 IVT
 │    按 IVT 加载 SCFW + U-Boot
 ▼
② SCFW 启动（跑在一个专用 Cortex-M4 上 —— 不是我们用的那两个！）
 │    职责：电源域、时钟、pad mux、**资源分区(RM)**、安全
 │    它是整颗 SoC 的管家，A核和 M4 想动任何外设都要通过 IPC 求它
 ▼
③ U-Boot（A核）→ 加载 Image.gz + dtb → 跳 Linux
 ▼
④ Linux 启动 → 解析 dtb → probe 驱动
 │    imx_rproc 看到 fsl,imx8qm-cm4 节点
 ▼
⑤ remoteproc 加载 M4 固件
 │    读 ELF → 按段的 p_paddr 往内存写
 │    地址翻译走 imx_rproc_att_imx8qm 表（TCML 0x1FFE0000→0x34FE0000 等）
 │    然后调 imx_sc_pm_cpu_start(SC_R_M4_0_PID0, 0x34fe0000)
 ▼
⑥ M4 从 0x34fe0000（TCML）开始执行固件
     main() → SCU RPC 初始化 → FreeRTOS → app_task
```

**为什么启动地址决定了链接脚本**：SCU 收到 `cpu_start(rsrc, addr)` 后直接把 M4 的 PC 设到 `addr`。
所以设备树写 `fsl,entry-address = <0x34fe0000>`（TCML 的系统地址），固件就**必须**是链接到 TCM
的那版（`build_debug.sh`）。DDR 版代码在 `0x88000000`，M4 会从空的 TCML 开始跑 → 死。见案例 3。

### 1.3 数据链路

以「CAN 收帧 → RPMsg 转发给 Linux」为例，从总线到终端：

```
CAN 总线
 │
 │ ① FlexCAN 控制器（0x5a8d0000）收到帧 → 硬件自动 ACK → 写进 Rx 邮箱 MB9
 ▼
② 中断：FlexCAN → INTMUX → IRQSTEER → NVIC → can_rx_callback()
 │    ⚠️ FreeRTOS 约束：走 IRQSTEER 的 ISR 优先级必须 ≥ 2（案例 5）
 ▼
③ ISR 只做搬运：把帧塞进 FreeRTOS 队列（xQueueSendFromISR）
 │    为什么不直接发 rpmsg？—— rpmsg_lite 不是 ISR-safe
 ▼
④ can_task 出队 → 格式化成字符串 → rpmsg_lite_send_nocopy()
 │
 │ ⑤ rpmsg_lite 把消息写进 vring（共享内存 0x90010000）
 │    再"敲门"(kick)：通过 MU 给 A 核发一个中断
 ▼
⑥ Linux 侧 virtio_rpmsg_bus 收到 kick → 从 vring 取数据
 │
 ▼
⑦ imx_rpmsg_tty 把数据推进 tty → /dev/ttyRPMSG30
```

#### 共享内存布局

RPMsg **不通过 MU 传数据本身**。MU 只传 32 位的通知，数据放在双方都能访问的共享内存里：

```
0x90000000  vdev0vring0   ┐ SRTM 通道（系统级）
0x90008000  vdev0vring1   ┘
0x90010000  vdev1vring0   ┐ USER 通道（firmware 里 RPMSG_LITE_SHMEM_BASE 用的这个）
0x90018000  vdev1vring1   ┘
0x900ff000  rsc-table     资源表
0x90400000  vdevbuffer    数据缓冲区（消息内容实际存放处）
```

**vring** = 环形缓冲区 + 描述符表，即 VirtIO 的标准结构，各 `0x8000`。

#### 四个关键机制

**① 为什么要"敲门"(kick)**

共享内存是"**拉**"模型 —— 数据写进去了，对方怎么知道？所以需要一个中断来"**推**"通知：

- M4 写 vring → 通过 MU 给 A 核发中断 → Linux 在中断里读 vring
- Linux 写 vring → 同样通过 MU 通知 M4

**"写共享内存 + 敲中断"这个组合，是所有核间通信框架的骨架。** 看到 `imx_rproc_kick` 就知道是这一环。

**② 资源表 (rsc_table) —— 发现机制**

固件里的 `resources` 结构体声明"我有几个 vring、在哪个地址、vdevbuffer 在哪"。启动时它被拷到
`0x900ff000`，同时作为 `.resource_table` 段留在 ELF 里。Linux 的 remoteproc 两种都能找到。

**这让 M4 固件可以独立演进，Linux 不需要预先知道布局。**

**③ 名字服务 (nameservice) —— 端点怎么对上**

M4 侧端点地址 30（`LOCAL_EPT_ADDR`），Linux 侧 `0x400`，但两边**不是硬编码配对**的：

```c
rpmsg_ns_announce(my_rpmsg, my_ept, "rpmsg-virtual-tty-channel", RL_NS_CREATE);
```

M4 **广播**"我提供叫 `rpmsg-virtual-tty-channel` 的服务"，Linux 收到后动态建 channel。
也正因为这个名字，`imx_rpmsg_tty`（匹配该名字）才会认领它，而 `rpmsg_tty`（匹配 `"rpmsg-tty"`）不会 —— 见案例 2。

**④ 零拷贝在哪**

`rpmsg_lite_alloc_tx_buffer()` 直接从 `vdevbuffer` 拿一块，`send_nocopy()` 只把它的**索引**写进
vring 描述符 —— **数据本身一次都没被复制过**。所以发完之后不能再用那个 buffer。

### 1.4 设计思路

| 设计选择 | 原因 |
|---|---|
| 数据放共享内存，不用 MU 传 | MU 只有 32 位寄存器，传大数据要反复搬运。共享内存 + 指针是零拷贝 |
| 用中断而不是轮询 | 省 CPU、降延迟；空闲时双方都能睡 |
| 有资源表而不是硬编码地址 | 固件可独立演进，Linux 不用跟着改 |
| 有名字服务而不是固定端点 | 支持多服务、动态创建/销毁 |
| RPMsg 架在 MU 之上 | MU 只是"物理层"；RPMsg 补齐了消息队列 + 端点 + 名字服务，这才是能用的抽象 |
| 外设归 SCU 管 | 安全与功耗隔离：一个分区崩溃不会搞乱别的分区 |

### 1.5 想继续深入

| 方向 | 看哪里 |
|---|---|
| 中断怎么从外设到 NVIC | `fsl_irqsteer.c`；`fsl_flexcan.c` 的 `FLEXCAN_TransferHandleIRQ` |
| MU 怎么工作 | `fsl_mu.c`；`rpmsg_platform.c` 的 `platform_init_interrupt` |
| vring 的格式 | `middleware/multicore/rpmsg_lite/lib/` 下的 `rpmsg_lite.c`、`virtqueue.c` |
| Linux 侧怎么解析 | `drivers/rpmsg/virtio_rpmsg_bus.c` |
| remoteproc 怎么加载 | `drivers/remoteproc/remoteproc_elf_loader.c`、`imx_rproc.c` |
| 资源表格式 | `cm4_core0/rsc_table.c` 与 `remoteproc.h`（TI 的标准头） |
| VirtIO 规范 | OASIS VirtIO 规范（vring 结构是公开标准） |

---

## 2. 开发环境

### 2.1 工具链

```bash
export ARMGCC_DIR=/home/cxy/work_local/IMX8/freertos-variscite-mcuxpresso_sdk_2.9.x-var01/gcc-arm-none-eabi-9-2020-q2-update
```

工具链在仓库内部，不在 `PATH` 上。`tools/cmake_toolchain_files/armgcc.cmake:16` 读不到就直接 Fatal Error。**每个新终端都要 export。**

### 2.2 目录结构

例程路径比别的板子**多一层 `cm4_core0`**（SOM-MX8QM 是全树唯一有双 Cortex-M4 的板子）：

```
boards/som_mx8qm/multicore_examples/rpmsg_lite_str_echo_rtos/
├── cm4_core0/                     ← core0
│   ├── main_remote.c              ← 主逻辑
│   ├── board.c / board.h          ← 板级初始化 + 板级宏
│   ├── pin_mux.c / pin_mux.h      ← pad 配置
│   ├── clock_config.c / .h        ← 时钟
│   ├── FreeRTOSConfig.h           ← ⚠️ 中断优先级、断言行为都在这里
│   ├── rsc_table.c / .h           ← remoteproc 资源表
│   ├── rpmsg_config.h
│   └── armgcc/                    ← 构建目录，只在这里编
└── cm4_core1/                     ← core1（编出来是 _m41）
```

### 2.3 构建系统：两套组件开关

MCUXpresso 的 CMake 用**两层**机制选源文件，缺一个就会 "undefined reference"：

**第一层 `armgcc/config.cmake`** —— 用 `CONFIG_USE_xxx` 声明组件：

```cmake
set(CONFIG_USE_driver_lpuart_MIMX8QM6_cm4_core0 true)
```

**第二层 `armgcc/CMakeLists.txt`** —— 用 `include(...)` 真正把源文件加进构建：

```cmake
include(driver_lpuart_MIMX8QM6_cm4_core0)
include(driver_flexcan_MIMX8QM6_cm4_core0)     ← 加 CAN 驱动就是加这一行
```

> **加新外设驱动的正确做法**：去 `boards/som_mx8qm/driver_examples/<外设>/` 找对应例程，
> 看它 `CMakeLists.txt` 里 `include()` 了哪些，照抄。
> 本次加 FlexCAN 就是抄 `driver_examples/flexcan/interrupt_transfer/cm4_core0/armgcc/CMakeLists.txt:70`。

**脚本不能跨目录复用** —— 里面的工具链相对路径写死了，层数还随嵌套深度变化。

### 2.4 六种构建配置

| 脚本 | 链接脚本 | 代码位置 | 用途 |
|---|---|---|---|
| `build_debug.sh` | `*_ram.ld` | **TCM** | ✅ 配合 `-m4` dtb 用这个 |
| `build_release.sh` | `*_ram.ld` | TCM | ✅ |
| `build_ddr_debug.sh` | `*_ddr_ram.ld` | DDR `0x88000000` | ❌ 见案例 3 |
| `build_ddr_release.sh` | `*_ddr_ram.ld` | DDR | ❌ |
| `build_flash_*.sh` | `*_flash.ld` | flash | ❌ |

---

## 3. 固件程序结构

### 3.1 启动流程

```
复位 → SystemInit → main()
  │
  ├─ BOARD_InitRpc()              ← 建立与 SCU 的 IPC（一切外设操作的前提）
  ├─ BOARD_InitPins(ipc)          ← pad mux + 电气配置
  ├─ BOARD_BootClockRUN()
  ├─ BOARD_InitDebugConsole()     ← LPUART4（这块板上看不到）
  ├─ BOARD_InitMemory()
  │
  ├─ sc_pm_set_resource_power_mode(SC_R_...)   ← 通过 SCU 给外设上电
  ├─ CLOCK_SetIpFreq(kCLOCK_..., SC_80MHZ)     ← 通过 SCU 设时钟
  ├─ IRQSTEER_Init(IRQSTEER)                   ← ⚠️ 不开 NVIC 优先级，见案例 5
  │
  ├─ copyResourceTable()          ← 资源表拷到 VDEV0_VRING_BASE
  ├─ xTaskCreate(app_task, ...)
  └─ vTaskStartScheduler()        ← 从这里开始才有任务
```

**关键分界**：`main()` 里跑的都是"上电初始化"，`app_task` 里才是业务。
本次加的 CAN 初始化放在 `main()`，收帧转发放在单独任务里。

### 3.2 RPMsg 是怎么起来的

`app_task` 里的核心四步：

```c
my_rpmsg = rpmsg_lite_remote_init((void *)RPMSG_LITE_SHMEM_BASE, LINK_ID, RL_NO_FLAGS);
while (0 == rpmsg_lite_is_link_up(my_rpmsg)) ;
my_queue = rpmsg_queue_create(my_rpmsg);
my_ept   = rpmsg_lite_create_ept(my_rpmsg, LOCAL_EPT_ADDR, rpmsg_queue_rx_cb, my_queue);
rpmsg_ns_announce(my_rpmsg, my_ept, "rpmsg-virtual-tty-channel", RL_NS_CREATE);
```

- `RPMSG_LITE_SHMEM_BASE` = `VDEV1_VRING_BASE`，core0 是 `0x90010000`（见 `board.h`）
- `LOCAL_EPT_ADDR` = 30 → Linux 侧设备节点名里的那个数字
- `rpmsg_ns_announce` 广播的名字**必须和 Linux 驱动匹配**（见案例 2）

> ⚠️ **`rpmsg_lite` 不是 ISR-safe**。中断里不能调 `rpmsg_lite_send_nocopy()`，
> 必须用队列/通知转到任务上下文再发。

### 3.3 完整示例：给 echo 例程加「CAN 收帧 → RPMsg 转发」

这是本次实际做的改动，可以直接当模板。改动全部在 `main_remote.c` + 一行 CMakeLists。

#### 第一步：SCU 侧初始化（在 `main()` 里，建任务之前）

```c
/* 1. SCFW 资源分区 —— 问 SCU「这块外设归我吗」 */
g_can.owned = sc_rm_is_resource_owned(ipc, SC_R_CAN_0) ? 1 : 0;

/* 2. 上电 */
g_can.power_on = (int32_t)sc_pm_set_resource_power_mode(ipc, SC_R_CAN_0, SC_PM_PW_MODE_ON);

/* 3. 时钟 */
g_can.clock = (int32_t)CLOCK_SetIpFreq(kCLOCK_DMA_Can0, SC_80MHZ);

/* 4. pad mux —— 值要抄设备树，不能抄例程（见案例 6） */
sc_pad_set_all(ipc, SC_P_FLEXCAN0_RX, 0U, SC_PAD_CONFIG_NORMAL, SC_PAD_ISO_OFF, 0x21, SC_PAD_WAKEUP_OFF);
sc_pad_set_all(ipc, SC_P_FLEXCAN0_TX, 0U, SC_PAD_CONFIG_NORMAL, SC_PAD_ISO_OFF, 0x21, SC_PAD_WAKEUP_OFF);
```

#### 第二步：控制器初始化

```c
FLEXCAN_GetDefaultConfig(&flexcanConfig);
flexcanConfig.baudRate               = 500000U;
flexcanConfig.timingConfig.phaseSeg1 = 6U;    /* quantum = 1+(6+1)+(4+1)+(6+1) = 20 */
flexcanConfig.timingConfig.phaseSeg2 = 4U;    /* 80MHz/(500k×20) = 8 → PRESDIV = 7 */
flexcanConfig.timingConfig.propSeg   = 6U;

FLEXCAN_Init(DMA__CAN0, &flexcanConfig, CLOCK_GetIpFreq(kCLOCK_DMA_Can0));
FLEXCAN_TransferCreateHandle(DMA__CAN0, &g_can_handle, can_rx_callback, NULL);

/* 全局掩码 0 = 不比较任何 ID 位 = 收全部标准帧 */
FLEXCAN_SetRxMbGlobalMask(DMA__CAN0, FLEXCAN_RX_MB_STD_MASK(0U, 0U, 0U));

mbConfig.format = kFLEXCAN_FrameFormatStandard;
mbConfig.type   = kFLEXCAN_FrameTypeData;
mbConfig.id     = FLEXCAN_ID_STD(0U);
FLEXCAN_SetRxMbConfig(DMA__CAN0, CAN_RX_MB, &mbConfig, true);

/* ⚠️ 见案例 5 */
for (i = 0; i < N; i++) NVIC_SetPriority(irqsteer_irqs[i], 3U);
IRQSTEER_EnableInterrupt(IRQSTEER, DMA_FLEXCAN0_INT_IRQn);
```

**波特率计算公式**（`FLEXCAN_SetBaudRate`，`fsl_flexcan.c:665`）：

```
quantum    = 1 + (PSEG1+1) + (PSEG2+1) + (PROPSEG+1)
preDivider = (sourceClock / (baudRate × quantum)) - 1
实际波特率  = sourceClock / ((preDivider+1) × quantum)
```

#### 第三步：ISR 只做搬运

```c
static void can_rx_callback(CAN_Type *base, flexcan_handle_t *handle,
                            status_t status, uint32_t result, void *userData)
{
    can_rx_frame_t f;
    BaseType_t     woken = pdFALSE;

    if ((status != kStatus_FLEXCAN_RxIdle) || (result != (uint32_t)CAN_RX_MB)) return;

    /* 驱动已经在回调前把邮箱内容拷进 g_can_frame 了 */
    f.id  = (g_can_frame.id & CAN_ID_STD_MASK) >> CAN_ID_STD_SHIFT;
    f.dlc = (uint8_t)g_can_frame.length;
    f.data[0] = (uint8_t)g_can_frame.dataByte0;   /* dataByte0..7 是语义命名 */
    ...
    g_can_rx_count++;                              /* 调试计数器，非常有用 */

    (void)xQueueSendFromISR(g_can_queue, &f, &woken);   /* ⚠️ 需要正确的 NVIC 优先级 */
    portYIELD_FROM_ISR(woken);
}
```

#### 第四步：任务里发送 + 重新装弹

```c
static void can_task(void *param)
{
    can_rx_frame_t f;
    for (;;)
    {
        if (xQueueReceive(g_can_queue, &f, portMAX_DELAY) != pdPASS) continue;

        can_rx_arm();                     /* 先重新装弹，再做慢活 */

        if ((g_rpmsg == NULL) || (g_ept == NULL) || (g_peer_addr == 0U)) continue;

        tx = rpmsg_lite_alloc_tx_buffer(g_rpmsg, &size, RL_BLOCK);
        if (tx == NULL) continue;

        p = app_put_str(tx, "CAN RX #");
        p = app_put_i32(p, (int32_t)g_can_rx_count);
        p = app_put_str(p, " id=0x");   p = app_put_hex(p, f.id);
        p = app_put_str(p, " dlc=");    p = app_put_i32(p, (int32_t)f.dlc);
        p = app_put_str(p, " data=");
        for (i = 0; i < f.dlc; i++) { if (i) *p++ = ' '; p = app_put_hex2(p, f.data[i]); }
        p = app_put_str(p, "\r\n");

        len = (uint32_t)(p - tx);
        if (len > size) len = size;       /* 防御越界 */
        (void)rpmsg_lite_send_nocopy(g_rpmsg, g_ept, g_peer_addr, tx, len);
    }
}
```

> **`g_peer_addr` 从哪来**：M4 只能从"收到的消息"里学到 Linux 的地址。
> 所以 `app_task` 每次 `rpmsg_queue_recv_nocopy` 之后把它存下来。
> 实测驱动在 probe 时会主动发一条 `"hello world!"`，所以链接建立后立刻就有值了。

#### 第五步：不要用 `snprintf`

```c
/* 这个工程里 newlib stdio 是唯一的外部依赖，手写更省心 */
static char *app_put_i32(char *p, int32_t v);      /* 十进制 */
static char *app_put_hex2(char *p, uint8_t v);     /* 两位十六进制 */
static char *app_put_hex32f(char *p, uint32_t v);  /* 八位十六进制，寄存器 dump 用 */
static char *app_put_str(char *p, const char *s);
```

好处：没有 newlib 依赖、**不存在 `snprintf` 返回值可能大于 buffer 导致越界的坑**、体积小 2 KB。

---

## 4. 编译与产物验证

```bash
cd .../boards/som_mx8qm/multicore_examples/rpmsg_lite_str_echo_rtos/cm4_core0/armgcc
export ARMGCC_DIR=...
./build_debug.sh
# → debug/rpmsg_lite_str_echo_rtos_imxcm4_m40.elf
```

### 4.1 常见编译错误

| 报错 | 原因 |
|---|---|
| `unknown type name 'flexcan_transfer_t'` | 类型名错了，实际是 `flexcan_mb_transfer_t`。**查 `fsl_xxx.h` 的定义，别猜** |
| `unknown type name 'QueueHandle_t'` | 缺 `#include "queue.h"`（`FreeRTOS.h` 不含队列 API） |
| `undefined reference to 'FLEXCAN_Init'` | `CMakeLists.txt` 里漏了 `include(driver_flexcan_...)` |
| `implicit declaration of 'xQueueSendFromISR'` | 同上，缺 `queue.h` |

### 4.2 上板之前能做的验证（很重要的习惯）

**不需要烧板子就能查出很多问题。**

```bash
TC=.../gcc-arm-none-eabi-9-2020-q2-update/bin
E=armgcc/debug/rpmsg_lite_str_echo_rtos_imxcm4_m40.elf

# 1) 新函数在不在
$TC/arm-none-eabi-nm $E | grep -E "can_init|can_task|can_rx_callback"

# 2) 驱动 API 是否真的链进来（T = 强符号，不是 weak 空壳）
$TC/arm-none-eabi-nm $E | grep -E " T FLEXCAN_(Init|SetRxMbConfig|TransferReceiveNonBlocking)"

# 3) 报文格式串在不在（确认代码确实进去了）
$TC/arm-none-eabi-strings $E | grep "CAN RX #"

# 4) 体积是否合理
$TC/arm-none-eabi-size $E

# 5) md5，用于和板子上那份对比
md5sum $E
```

#### 中断向量链验证（本次最有用的技巧）

NXP 的启动文件给每个外设中断都定义了 **weak 空壳**，真正的实现由驱动提供。如果驱动的实现没链进来，中断会落到 `DefaultISR` 死循环 —— **而且编译器不报错**。

```bash
# 看链的末端是不是强符号（T）
$TC/arm-none-eabi-nm $E | grep -E "DMA_FLEXCAN0_INT_(IRQHandler|DriverIRQHandler)|DefaultISR"
```

期望：

```
1ffe0c1c W DefaultISR                            ← 默认死循环
1ffe9208 T DMA_FLEXCAN0_INT_DriverIRQHandler     ← T ！驱动接管了
1ffe0cd0 W DMA_FLEXCAN0_INT_IRQHandler           ← 启动文件的小跳板
```

链路是：**向量表 → weak `..._IRQHandler`（跳板）→ `..._DriverIRQHandler`（驱动强符号）→ 驱动 ISR**。

想确认跳板跳到哪，用 objdump 反汇编 + 解字面量池：

```bash
$TC/arm-none-eabi-objdump -d $E --start-address=0x1ffe0cd0 --stop-address=0x1ffe0cd4
# 1ffe0cd0:  48c9  ldr  r0, [pc, #804]   ; (1ffe0ff8 ...)
# 1ffe0cd2:  4700  bx   r0
$TC/arm-none-eabi-objdump -s -j .text $E --start-address=0x1ffe0ff8 --stop-address=0x1ffe0ffc
# 1ffe0ff8 0992fe1f      → LE 0x1FFE9209 → &~1 = 0x1FFE9208 = 驱动的 handler ✅
```

> ⚠️ 字面量池里每个入口对应**附近**的一个 stub，别按地址顺序想当然 —— 本次我就认错过一次。

#### 程序头 / 入口地址

```bash
$TC/arm-none-eabi-readelf -l $E      # 入口点 + 各 LOAD 段的 VMA/LMA
$TC/arm-none-eabi-readelf -S $E | grep resource_table
```

TCM 版应为：entry `0x1ffe0b5d`、`.interrupts` @ `0x1ffe0000`、代码在 TCML、data/bss 在 TCMU `0x20000000`。

---

## 5. 调试手段

### 5.1 三层观测点，可靠性从高到低

| 观测点 | 看得到什么 | 可靠性 |
|---|---|---|
| **M4 的 `PRINTF`（LPUART4 串口）** | M4 内部状态、`assert` 报错、执行轨迹 | ⭐⭐⭐ **最直接** —— 但需要外接一根串口线。**2026-09-17 已实测接上可用**，配置本身是正确完整的 |
| **`dmesg`（`rpmsg_tty_cb`）** | 内核**实际收到**的每一个字节 | ⭐⭐⭐ 不需要接线 |
| `/dev/ttyRPMSG*` + `cat` | 用户态读到的数据 | ⭐⭐ 受 tty 行规程影响 |

> **串口是最终解，rpmsg 是没线时的替代。** 两者互补：串口能看到启动早期（rpmsg 端点建立之前）和
> 崩溃现场；rpmsg 不需要线。理想状态是两个都留着。
>
> 但注意：FreeRTOS 的 `configASSERT` 在这个工程里是 `taskDISABLE_INTERRUPTS(); for(;;);` ——
> **静默的，走哪条路都看不到提示**。真要抓那种情况只能上 J-Link。

**核心方法论：判断"M4 到底发没发"，一律以 `dmesg` 为准。**

`imx_rpmsg_tty` 驱动里 `rpmsg_tty_cb()` 会对收到的数据做 `print_hex_dump`，所以：

```bash
dmesg -C
echo "can" > /dev/ttyRPMSG30
sleep 1
dmesg | grep rpmsg_tty_cb
```

每行左边是十六进制、**右边那一列就是明文**，报告会被切成 16 字节一行。

**为什么不能用 `cat` 判断**：
1. tty 默认是**规范模式（ICANON）**，`cat` 会阻塞且不一定及时吐出数据 → 用 `timeout 3 cat /dev/ttyRPMSG*` 或 `stty -F /dev/ttyRPMSG* raw -echo`
2. **`/dev` 下可能有个同名的普通文件挡着**（见案例 1），`cat` 读到的是文件内容而不是 M4 的数据 —— 本次就在这里被骗了很久

### 5.2 判断设备节点是否正常

```bash
ls -l /dev/ttyRPMSG30
# crw-rw----  ... 507, 0 ...   ← 'c' 开头才是字符设备 ✅
# -rw-r--r--  ...              ← '-' 开头是普通文件 ❌

# 内核认为它该是什么样
cat /sys/class/tty/ttyRPMSG30/dev      # 输出 "major:minor"
```

**这条检查应该放在最前面。** 本次因为跳过了它，浪费了大量时间。

### 5.3 「固件内建诊断命令」模式 ← 强烈推荐

没有串口线时，**让 M4 自己把状态通过 RPMsg 吐出来**。这是本次最有效的调试手段 ——
整个 CAN 问题（案例 5）就是靠它定位的，全程没接线。

在 `app_task` 的收包分支里加一个触发词：

```c
if ((len >= 3) && (app_buf[0] == 'c') && (app_buf[1] == 'a') && (app_buf[2] == 'n'))
{
    /* 回一份实时状态报告，而不是回显 */
    p = app_put_str(p, "CAN0 STATUS\r\n  owned=");
    p = app_put_i32(p, g_can.owned);
    ...
    p = app_put_str(p, "\r\n  MCR=0x");   p = app_put_hex32f(p, DMA__CAN0->MCR);
    p = app_put_str(p, " CTRL1=0x");      p = app_put_hex32f(p, DMA__CAN0->CTRL1);
    p = app_put_str(p, "\r\n  ESR1=0x");  p = app_put_hex32f(p, DMA__CAN0->ESR1);
    p = app_put_str(p, " IFLAG1=0x");     p = app_put_hex32f(p, DMA__CAN0->IFLAG1);
    p = app_put_str(p, "\r\n  rx_count="); p = app_put_i32(p, (int32_t)g_can_rx_count);
}
```

实测输出：

```
CAN0 STATUS
  owned=1 power_on=0 clock=80000000
  pad_rx=0 pad_tx=0
  MCR=0x04a0000f CTRL1=0x0774cc26
  ESR1=0x00040080 IFLAG1=0x00000000
  rx_count=0 pending_in_queue=0
```

**怎么读**（位定义查 `devices/MIMX8QM6/MIMX8QM6_cm4_core0.h` 的 `CAN_xxx_MASK/SHIFT`，别凭记忆）：

| 字段 | 含义 |
|---|---|
| `owned` | `sc_rm_is_resource_owned()` — SCFW 有没有把资源划给 M4 分区 |
| `power_on` / `pad_*` | `sc_err_t`，`0` = `SC_ERR_NONE` |
| `clock` | `CLOCK_GetIpFreq()` 的返回值，波特率算错多半是这里不对 |
| `MCR` bit31 `MDIS` | 0 = 模块已使能 |
| `CTRL1` bit26:24 `PRESDIV` | 应为 7；PSEG1/PSEG2/PROPSEG 在 bit21:19 / 18:16 / 2:0 |
| `ESR1` bit4:5 `FLTCONF` | 0=error active；非 0 → 总线问题 |
| `ESR1` bit7 `IDLE` | 1 = 控制器看到的总线是空闲的 |
| `ESR1` 各错误位 | `STFERR`/`FRMERR`/`CRCERR`/`ACKERR` — 非 0 说明总线上有异常帧 |
| `IFLAG1` | 邮箱中断标志，非 0 说明有数据没被处理 |
| **`rx_count`** | **中断里累加的收帧数 — 最能说明问题的一个数** |

> **位定义必须查头文件。** 我第一次凭记忆读 `CTRL1`，把 `PRESDIV` 读成了错的位，
> 差点得出"时序配错了"的错误结论。查了 `CAN_CTRL1_PRESDIV_SHIFT` 才发现是对的。

### 5.4 remoteproc 状态

```bash
ls /sys/class/remoteproc/               # 有几个核
cat /sys/class/remoteproc/remoteproc0/name      # imx8qm-cm4-0
cat /sys/class/remoteproc/remoteproc0/state     # offline / running
cat /sys/class/remoteproc/remoteproc0/firmware  # ⚠️ 重启后会被清空
```

**注意**：`state = running` **不代表 M4 活着** —— remoteproc 没有 watchdog，M4 崩了它照样显示 running。

**M4 挂掉的可观测信号**：

```
imx-rproc imx8qm-cm4-0: imx_rproc_kick: failed (3, err:-62)
```

`-62` = `-ETIME`。Linux 想通过 MU 通知 M4，但 MU 一直"忙"（M4 没读）→ `mbox_send_message` 超时。
**看到这行基本可以判定 M4 卡住了。** 本次就是靠它抓到案例 5 的。

---

## 6. 实战案例：本次踩的坑

**按"教训价值"排序，不是按编号顺序** —— 编号沿用正文其它章节里的交叉引用（`见案例 N`）。
所以顺序是 5 → 1 → 2 → 3 → 6。

### 案例 5（最隐蔽）：中断优先级不够 → configASSERT 静默死循环

**现象**

M4 起来后一切正常，能回显消息。**一旦有 CAN 帧到达就彻底失联** —— 之后再发什么都不应，dmesg 里出现 `imx_rproc_kick: failed (3, err:-62)`。

**排查路径**（这个过程本身值得学）

1. **先排除"自发崩溃"**：重启 M4，什么都不做等 60 秒，再查询 → **正常回报告**。所以不是自发的。
2. **锁定触发条件**：对比"发 CAN 帧前 / 后" → 死在收到帧时。**推论：帧其实到达了控制器**，是接收路径把 M4 弄死的。
3. **看 `FreeRTOSConfig.h`**：
   ```c
   #define configASSERT_BOOL(x) if((x)==0) { taskDISABLE_INTERRUPTS(); for (;;); }
   #define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 2
   #define configMAX_SYSCALL_INTERRUPT_PRIORITY (2 << (8-4))   /* 0x20 */
   ```
   **`configASSERT` 的失败方式是「关中断 + 死循环」** —— 没有输出、没有重启，完美解释"失联"。
4. **查 MU 为什么没事**：`middleware/.../imx8qm_m4/rpmsg_platform.c:450`
   ```c
   NVIC_SetPriority(APP_M4_MU_NVIC_IRQn, APP_MU_IRQ_PRIORITY);   /* = 3 */
   ```
   MU 的优先级是**显式设过的**。
5. **查 IRQSTEER**：`fsl_irqsteer.c` 的 `IRQSTEER_Init()` 只调 `EnableIRQ()`，**不设优先级** → 复位默认 **0**。
   而 `0x00 < 0x20` → ISR 里调 `xQueueSendFromISR()` 触发断言。

**根因**

```c
/* 中断优先级数值 < configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY(2) 的 ISR 里，
   不能调用任何 ...FromISR() 的 FreeRTOS API */
```

**修复**

```c
static const IRQn_Type irqsteer_irqs[] = IRQSTEER_IRQS;
for (i = 0; i < sizeof(irqsteer_irqs)/sizeof(irqsteer_irqs[0]); i++)
    NVIC_SetPriority(irqsteer_irqs[i], 3U);      /* 和 MU 保持一致 */
IRQSTEER_EnableInterrupt(IRQSTEER, DMA_FLEXCAN0_INT_IRQn);
```

**教训**

- **`configASSERT` 是静默杀手**：表现为"对方不理我了"，极易误判成硬件问题或对方死机
- **规律**：这个工程里凡是要走 IRQSTEER 的中断，只要 ISR 里调 FreeRTOS 的 `...FromISR()`，就必须先设 NVIC 优先级 ≥ 2。`IRQSTEER_Init()` 不会替你做
- **`imx_rproc_kick: failed -ETIME` 是"M4 卡住"的可靠信号**，比 `state` 有用得多

---

### 案例 1：`/dev` 下有个同名普通文件挡住了设备节点

**现象**

`cat /dev/ttyRPMSG30` **能读到内容**，但 dmesg 里**没有对应的 `rpmsg_tty_cb`**。看起来"往返成功"，实际全是假象。

**排查**

```bash
ls -l /dev/ttyRPMSG30
# -rw-r--r-- 1 root root 17 ... /dev/ttyRPMSG30      ← '-' 开头！是普通文件
file /dev/ttyRPMSG30
# /dev/ttyRPMSG30: ASCII text
cat /proc/mounts | grep " /dev "                    # devtmpfs
cat /sys/class/tty/ttyRPMSG30/dev                   # 507:0  ← 内核侧设备是好的
```

**根因**

某个时刻（M4 停着、节点还不存在时）执行了 `echo "..." > /dev/ttyRPMSG30`。
**root 在 devtmpfs 的 `/dev` 下写一个不存在的路径，会直接创建一个普通文件**，不报错。
之后内核 `device_create()` 想建真正的节点时撞名失败，这个假文件就一直占着位置。

**修复**

```bash
rm -f /dev/ttyRPMSG30            # 必须先删，否则 devtmpfs 建不出节点
echo stop  > /sys/class/remoteproc/remoteproc0/state
echo start > /sys/class/remoteproc/remoteproc0/state
ls -l /dev/ttyRPMSG30            # 变成 crw-...c 就成了
```

**教训**

- **判断设备节点，第一件事就是 `ls -l` 看首字符是 `c` 还是 `-`**
- `echo > /dev/不存在的设备` **会静默创建文件**，不报错
- 重启后 `/dev` 是干净的（devtmpfs 会清空），这个坑自动消失

---

### 案例 2：装错驱动 —— `rpmsg_tty` vs `imx_rpmsg_tty`

**现象**

dmesg 显示 `virtio_rpmsg_bus virtio1: creating channel rpmsg-virtual-tty-channel addr 0x1e`（M4 侧名字服务握手成功），但 `/dev/ttyRPMSG*` 一直不出现。

**排查**

```bash
grep -rn "rpmsg_device_id" drivers/tty/rpmsg_tty.c drivers/rpmsg/imx_rpmsg_tty.c
```

| 模块 | 匹配的 channel 名 |
|---|---|
| `rpmsg_tty`（`drivers/tty/`） | `"rpmsg-tty"` ❌ |
| `imx_rpmsg_tty`（`drivers/rpmsg/`） | `"rpmsg-virtual-tty-channel"` ✅ |

**根因**

M4 在 `main_remote.c:30` announce 的是 `RPMSG_LITE_NS_ANNOUNCE_STRING = "rpmsg-virtual-tty-channel"`，
只有 NXP 那个驱动会认领。装 `rpmsg_tty` 不会有任何报错，只是静静地看着。

**修复**

```bash
rmmod rpmsg_tty 2>/dev/null
modprobe imx_rpmsg_tty
```

**教训**

- 名字相近的驱动**查 `id_table` 而不是猜**
- 设备节点名来自 `imx_rpmsg_tty.c:151` 的 `kasprintf("ttyRPMSG%d", rpdev->dst)` —— **以 `ls` 实际为准**，别硬编码 30

---

### 案例 3：DDR 版固件配 `-m4` 设备树

**现象**

`start` 成功，M4 起来了，RPMsg 端点也建了，但外设行为不对（本次是 CAN 完全无反应）。

**排查**

```bash
# 看设备树期望的启动地址
grep -n "entry-address" /tmp/board_m4.dts
# fsl,entry-address = <0x34fe0000>       ← TCML 的【系统】地址

# 看固件的入口和段地址
arm-none-eabi-readelf -l debug/*.elf
```

| 版本 | entry | 代码位置 |
|---|---|---|
| `build_debug.sh`（TCM） | `0x1ffe0b5d` | TCML `0x1ffe0000` → 系统 `0x34fe0000` ✅ |
| `build_ddr_debug.sh`（DDR） | `0x88000b5d` | DDR `0x88000000` ❌ |

**根因**

驱动 `imx_rproc.c:1080` 读设备树的 `fsl,entry-address`，`:427` 原样传给 SCU 启动指令。
SCU 让 M4 从 `0x34fe0000`（TCML 系统地址）开始执行 —— DDR 版固件那里什么都没有，M4 跑空内存。

**修复**：`./build_debug.sh`

**教训**

**链接脚本的选择由设备树的 `fsl,entry-address` 决定，不是随便挑的。**
地址翻译规则见 `imx_rproc.c` 的 `imx_rproc_att_imx8qm` 表：

| 区域 | M4 视角 | 系统视角 |
|---|---|---|
| TCML | `0x1FFE0000` | `0x34FE0000` (core0) / `0x38FE0000` (core1) |
| TCMU | `0x20000000` | `0x35000000` / `0x39000000` |
| DDR | `0x80000000` | `0x80000000`（identity） |

---

### 案例 6：pad 电气配置要抄设备树，不能抄例程

**背景**

MCUXpresso 例程（`driver_examples/flexcan/.../pin_mux.c:75`）里 CAN0 的 pad 配置是：

```c
sc_pad_set_all(ipc, BOARD_BB_CAN0_RX_PIN_FUNCTION_ID, 0U, SC_PAD_CONFIG_NORMAL,
               SC_PAD_ISO_OFF, 0x40, SC_PAD_WAKEUP_OFF);     /* ← 0x40 */
```

而这块板的设备树里是：

```
flexcan0grp {
    fsl,pins = <0x93 0x00 0x21   0x92 0x00 0x21>;     /* <pin mux ctrl>  */
}
```

**`0x40` vs `0x21` —— pad 的电气配置（驱动强度/上拉等）不一样。**

**注意两个 API 不是一回事**：

| 函数 | RPC | 语义 |
|---|---|---|
| `sc_pad_set_all(ipc, pad, mux, config, iso, ctrl, wakeup)` | `PAD_FUNC_SET_ALL` | 参数分解传，**SCFW 自己补 enable 位** |
| `sc_pad_set(ipc, pad, val)` | `PAD_FUNC_SET` | 收完整寄存器值，**调用方自己或上 enable 位** |

Linux 的 `pinctrl-scu.c:118` 用的是后者，所以它要自己算 `val = conf | BM_PAD_CTL_IFMUX_ENABLE | BM_PAD_CTL_GP_ENABLE`。
**用 `sc_pad_set_all` 时不需要管那两位**，只需要把 ctrl 字段抄对。

**教训**：pad 配置值**以板子设备树为准**（那是 Linux 下验证过的），例程的值可能是别的基板变体的。

> 坦白说：本次改成 `0x21` 后**没有观测到任何差异**，真正的 bug 是案例 5。
> 但从"已知可工作的配置"出发更稳妥。如果你怀疑某个 pad 配置，正确做法是
> **反编译板子的 dtb，找出对应 pinctrl 组，逐字段对照**。

---

### 反面案例：三次误判 —— 以及它们的共同原因

这一节比上面任何一个都重要。

**误判 1：怀疑 CAN 收发器没使能**

推理："帧收不到，又没有错误 → 收发器没把信号送过来"。
还花时间查了 MEK 的 `xceiver-supply`、例程里那段 "Configure CAN I/O Expander" 的 I2C 代码。

**实际**：收发器完全正常。帧一直都能到达控制器 —— 只是第一帧一到 M4 就 assert 死了。

**误判 2：怀疑 `snprintf` 把 M4 弄挂**

推理："M4 收到 `can` 之后就不回了 → 我新加的 `snprintf` 分支有问题"。
还为此重写了一版手写格式化（改动本身是好的，但**不是修 bug**）。

**实际**：那段时间用户看到的 `can` 输出**是 `/dev` 下那个普通文件的内容**（案例 1），
消息根本没送到 M4。所谓"M4 收到后挂掉"这个前提就是错的。

**误判 3：怀疑 pad 的 `0x40`**

同案例 6 —— 改了，但没观测到差异，不是根因。

**共同原因**

> **在"数据到底有没有到达 M4"没有被独立证实之前，就往下游推理。**

三次都是在"M4 收到了/没收到"这个前提没钉死的情况下，直接去猜 M4 内部的代码问题。

**教训（这条方法论值得背下来）**

1. **先把"输入是否送达"和"输出是否产生"分别钉死**，再谈中间的处理逻辑
   - 输入送达的证据：`rx_count` 涨、`IFLAG1` 置位、`ESR1` 变化
   - 输出产生的证据：`dmesg` 里的 `rpmsg_tty_cb`
2. **选对观测点**：`cat` 会骗你（案例 1），`dmesg` 不会
3. **发现"某个改动没产生任何可观测差异"时，要立刻回头质疑前提**，而不是继续在上面加改动
4. **把假设做成可测量的形式**。本次有效的做法是：
   - 加 `rx_count` 计数器 → 一刀切开"帧到没到控制器"
   - 加实时寄存器 dump → 一刀切开"配置对不对"
   - 空闲 60 秒测试 → 一刀切开"自发崩溃"和"交互触发"
   - "发帧前后各查一次" → 定位触发时刻

---

## 7. 排错决策树

```
M4 起不来 / 行为异常
│
├─ 1. /sys/class/remoteproc/ 是空的？
│     └─ 是 → 设备树没生效，跟固件无关
│           检查: ls /proc/device-tree | grep cm4
│                 cat /sys/firmware/devicetree/base/model   （带 +M4 后缀才是 -m4 版）
│
├─ 2. state 是 offline 还是 detached？
│     └─ detached → SCFW 没把 M4 划给 Linux，固件再对也没用
│
├─ 3. echo start 报错？
│     ├─ "Invalid argument"         → 本来就是 offline，没啥可停（正常）
│     └─ "No such file or directory" → firmware 属性是空的（重启后会被清空）
│                                      要先 echo <名字>.elf > .../firmware
│
├─ 4. M4 起来了（端点建了）但功能不对？
│     ├─ 先看 rmmod/modprobe 对不对（案例 2）
│     ├─ 再看设备节点类型 ls -l（案例 1）
│     └─ 再用 dmesg | grep rpmsg_tty_cb 而不是 cat（§5.1）
│
├─ 5. 固件内部状态不明？
│     └─ 加"诊断命令"模式，把状态结构体和寄存器 dump 出来（§5.3）
│
└─ 6. M4 毫无征兆地失联？
      ├─ dmesg 里有 imx_rproc_kick: failed → M4 卡住了
      ├─ 先测"空闲会不会自己死" → 区分自发/交互触发
      ├─ 查 FreeRTOSConfig.h 的 configASSERT 行为
      └─ 查所有走 IRQSTEER 的 ISR：NVIC 优先级设了吗？（案例 5）
```

---

## 8. 关键常量速查

### 8.1 地址

| 项目 | 值 | 出处 |
|---|---|---|
| M4 core0 的 VDEV0 / VDEV1（vring） | `0x90000000` / `0x90010000` | `cm4_core0/board.h:107` |
| M4 core1 的 VDEV0 / VDEV1 | `0x90100000` / `0x90110000` | 同文件 `:111` |
| 资源表偏移 | `0xFF000`（→ `0x900ff000`） | `board.h:116` |
| shared-dma-pool | `0x90400000`，长 `0x100000` | 设备树 |
| TCML / TCMU（系统视角） | `0x34FE0000` / `0x35000000` | `imx_rproc_att_imx8qm` |
| M4 启动地址（`fsl,entry-address`） | `0x34fe0000` | 设备树 |
| CAN0 / CAN1 / CAN2 | `0x5A8D0000` / `0x5A8E0000` / `0x5A8F0000` | `MIMX8QM6_cm4_core0.h:9818` |

### 8.2 关键 SCFW 资源 ID（`scfw_api/main/types.h`）

| 名称 | 值 | 用途 |
|---|---|---|
| `SC_R_CAN_0` | 105 | CAN0 |
| `SC_R_M4_0_PID0` | 278 | M4_0 本体，用于资源归属判断 |
| `SC_R_M4_0_MU_1A` | 297 | M4_0 的 MU |
| `SC_R_MU_5A` / `SC_R_MU_6A` | 218 / 219 | core0 / core1 的 MU |
| `SC_R_IRQSTR_M4_0` | — | M4_0 的中断聚合器 |

### 8.3 pad 常量（`scfw_api/main/imx8qm_pads.h`）

| 名称 | 值 |
|---|---|
| `SC_P_FLEXCAN0_RX` | 146 |
| `SC_P_FLEXCAN0_TX` | 147 |

### 8.4 本次实测固件

```
boards/som_mx8qm/multicore_examples/rpmsg_lite_str_echo_rtos/cm4_core0/armgcc/debug/
  rpmsg_lite_str_echo_rtos_imxcm4_m40.elf

md5 5db7ac14b150cfe729a3eae9dbb6caeb     (含 CAN 收帧转发 + IRQ 优先级修复)
text 48688 B，TCML 128 KB 内，宽裕
```

---

## 9. 已知局限（当前固件）

| 局限 | 影响 | 要改的话 |
|---|---|---|
| 只用 **1 个 Rx 邮箱**，重新装弹在任务里做 | 突发大量帧时**可能丢帧** | 改用 **Rx FIFO**（`MCR.RFEN=1` + `FLEXCAN_SetRxFifoGlobalMask`） |
| `g_peer_addr` 要有过一次收发才非 0 | 链接建立前的第一帧会被丢弃 | 驱动 probe 时会发 `hello world!`，实际不会发生 |
| 未做开机自动拉起 | 每次重启要手动 `echo start` | dtb 加 `firmware-name`（需先确认 `imx_rproc` 支持），或写 systemd unit |
| `can_task` 栈 256 words | 目前够用 | 加功能时留意 |

---

## 10. 参考

- 上板运行流程：[`SOM-MX8QM_M4_RPMSG_GUIDE.md`](SOM-MX8QM_M4_RPMSG_GUIDE.md)
- 外设例程模板：`boards/som_mx8qm/driver_examples/<外设>/<例程>/cm4_core{0,1}/`
- 驱动源码：`devices/MIMX8QM6/drivers/fsl_*.c`
- SCFW API：`devices/MIMX8QM6/scfw_api/svc/`
- 寄存器位定义：`devices/MIMX8QM6/MIMX8QM6_cm4_core0.h`
