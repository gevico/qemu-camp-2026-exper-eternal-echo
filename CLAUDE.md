# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 提供此代码库的指导。

## 概述

这是 QEMU 训练营 2026 专业阶段实验仓库，包含四个 RISC-V 实验方向：CPU (TCG)、SoC (QTest)、GPGPU (QTest)、Rust (QTest + 单元测试)。G233 虚拟机是 CPU 和 SoC 实验的核心平台。

## 构建系统

使用 `Makefile.camp` 进行所有操作：

```bash
make -f Makefile.camp configure    # 配置 QEMU (riscv64-softmmu + linux-user + rust)
make -f Makefile.camp build        # 编译 QEMU
make -f Makefile.camp test-soc     # 运行 SoC 测试 (QTest)
make -f Makefile.camp test-cpu     # 运行 CPU 测试 (TCG)
make -f Makefile.camp test-gpgpu   # 运行 GPGPU 测试
make -f Makefile.camp test-rust    # 运行 Rust 测试
make -f Makefile.camp test         # 运行所有测试
```

构建目录是 `build/`，所有产物都在那里。

## 运行单个测试

SoC 实验（10 个测试在 `tests/gevico/qtest/`）：

```bash
# 运行特定测试
make -C build/tests/gevico/qtest/ run-board-g233
make -C build/tests/gevico/qtest/ run-gpio-basic

# 用 GDB 调试
make -C build/tests/gevico/qtest/ gdbstub-board-g233
# 然后在另一个终端：
gdb -ex "target remote :1234" build/qemu-system-riscv64
```

测试名遵循 `run-*` 模式，用 tab 补全查看可用测试。

## 仓库结构

```
qemu-camp-2026-exper-eternal-echo/
├── hw/                           # 硬件设备模型
│   ├── riscv/                    # RISC-V 板级实现
│   │   ├── g233.c                # G233 机器实现 (77KB) ← 核心文件
│   │   ├── virt.c                # 标准 virt 机器 (参考)
│   │   └── boot.c                # RISC-V 启动辅助
│   ├── gpio/                     # GPIO 设备
│   │   └── g233_gpio.c           # G233 GPIO 控制器
│   ├── char/                     # 字符设备 (UART 等)
│   ├── intc/                     # 中断控制器 (PLIC, ACLINT)
│   ├── timer/                    # 定时器设备 (PWM 等)
│   └── watchdog/                 # 看门狗设备
├── tests/                        # 测试套件
│   ├── gevico/                   # 训练营专用测试
│   │   ├── qtest/                # QTest 框架测试 (SoC/Rust)
│   │   │   ├── test-board-g233.c     # 实验 1
│   │   │   ├── test-gpio-basic.c      # 实验 2
│   │   │   ├── test-gpio-int.c        # 实验 3
│   │   │   ├── test-pwm-basic.c       # 实验 4
│   │   │   ├── test-wdt-timeout.c     # 实验 5
│   │   │   ├── test-spi-*.c           # 实验 6-10
│   │   │   └── libqtest.c             # QTest 框架库
│   │   └── tcg/                     # TCG 测试 (CPU 实验)
│   │       └── riscv64/
│   │           └── test-insn-*.c      # 指令测试
│   └── qtest/                     # 上游 QTest 测试
├── docs/                         # 文档 ← 实验参考
│   ├── note -> ~/Documents/.../QEMU  # 用户学习笔记 (符号链接)
│   └── tutorial/                 # 实验手册和数据手册
│       └── docs/exercise/2026/stage1/
│           ├── soc/              # SoC 实验文档
│           │   ├── g233-exper-manual.md   # 实验要求
│           │   └── g233-datasheet.md      # 硬件规格
│           ├── cpu/              # CPU 实验文档
│           ├── gpu/              # GPGPU 实验文档
│           └── rust/             # Rust 实验文档
├── include/hw/                   # 硬件头文件
│   └── riscv/
│       └── g233.h                # G233 机器定义
├── rust/                         # Rust 设备实现
│   └── hw/i2c/                   # Rust I2C 实验
├── build/                        # 构建输出 (生成)
│   └── tests/gevico/qtest/       # 编译后的测试二进制
├── Makefile.camp                 # 训练营构建系统
└── configure                     # QEMU 配置脚本
```

## 用户学习笔记

**符号链接**: `docs/note` → `/home/henry/Documents/Notes/obsidian-note/1.Project/Career/Upgrading/PLCT/QEMU/`

这是用户的 Obsidian 笔记目录（会持续迭代），记录实验进度和技术总结：

```
QEMU/
├── course/                      # 课程笔记
│   ├── qemu-tutor.md            # QEMU 教程
│   ├── soc/
│   │   ├── g233-datasheet.md    # G233 数据手册
│   │   └── note/                # SoC 实验笔记
│   │       ├── G233-实验笔记.md  # ⭐ 实验进度跟踪
│   │       ├── blog-01-仓库概述与深度解析.md
│   │       ├── blog-02-实验一详解.md
│   │       └── blog-QEMU设备模型实验从零到一.md
│   └── bak/
│       └── system-sim-peripheral-modeling-roadmap.blog.md
├── gpu/                         # GPU 方向笔记
└── rust/                        # Rust 方向笔记
```

**用途**：
- `G233-实验笔记.md`: 实验进度状态、常见问题、技术要点
- `blog-*.md`: 详细的实验博客，按实验编号组织
- 参考这些笔记了解用户的实验进展和已解决的问题

**注意**: 笔记内容会持续更新，查看最新版本直接访问 `docs/note/` 目录。

## 实验文档架构详解

`docs/tutorial/` 目录是实验的核心参考资料，采用 **MkDocs** 静态网站生成，完整的文档结构如下：

### 完整目录树

```
docs/tutorial/
├── docs/
│   ├── exercise/                 # 实验文档 ← 重点
│   │   ├── 2026/
│   │   │   ├── stage0/           # 导学阶段 (入门)
│   │   │   │   ├── learning-c.md      # C 语言学习指南
│   │   │   │   └── learning-rust.md   # Rust 语言学习指南
│   │   │   └── stage1/           # 专业阶段
│   │   │       ├── soc/          # SoC 方向
│   │   │       │   ├── index.md
│   │   │       │   ├── g233-exper-manual.md   # 实验手册 (必读)
│   │   │       │   └── g233-datasheet.md      # 硬件规格 (查阅)
│   │   │       ├── cpu/          # CPU 方向
│   │   │       │   ├── cpu-exper-manual.md
│   │   │       │   └── cpu-datasheet.md
│   │   │       ├── gpu/          # GPGPU 方向
│   │   │       │   ├── gpu-exper-manual.md
│   │   │       │   └── gpu-datasheet.md
│   │   │       └── rust/         # Rust 方向
│   │   │           ├── rust-exper-manual.md
│   │   │           └── rust-lang-manual.md
│   ├── tutorial/                 # 教学章节 (基础理论)
│   │   ├── 2026/
│   │   │   ├── ch0/              # 第 0 章：环境与资源
│   │   │   │   ├── qemu-dev-env.md       # QEMU 开发环境搭建
│   │   │   │   ├── gpu-history.md         # GPU 发展历史
│   │   │   │   └── gpgpu-resources.md     # GPGPU 学习资源
│   │   │   ├── ch1/              # 第 1 章：QEMU 基础概念
│   │   │   │   ├── qemu-qom.md           # QOM 对象模型
│   │   │   │   ├── qemu-mr.md            # 内存区域 (MemoryRegion)
│   │   │   │   ├── qemu-init.md          # 初始化流程
│   │   │   │   ├── qemu-debug.md         # 调试技巧
│   │   │   │   └── vm-history.md         # 虚拟机历史
│   │   │   ├── ch2/              # 第 2 章：QEMU 核心机制
│   │   │   │   ├── qemu-tcg.md           # TCG 前端编译器
│   │   │   │   ├── qemu-machine.md       # Machine 对象
│   │   │   │   ├── qemu-hw.md            # 设备模型
│   │   │   │   ├── qemu-intr.md          # 中断系统
│   │   │   │   ├── qemu-cpu-model.md     # CPU 模型
│   │   │   │   ├── qemu-clock.md         # 时钟管理
│   │   │   │   ├── qemu-gpgpu.md         # GPGPU 架构
│   │   │   │   ├── qemu-rust-*.md        # Rust 设备建模
│   │   │   │   └── qemu-insn.md          # 指令翻译
│   │   │   └── ch3/              # 第 3 章：案例分析
│   │   │       ├── qemu-k230.md         # K230 芯片模拟
│   │   │       ├── qemu-cxlemu.md        # CXL 模拟器
│   │   │       └── qemu-wine-ce.md       # Wine on ARM
│   │   └── 2025/              # 2025 年教程 (历史参考)
│   ├── blogs/                    # 博客文章 (经验分享)
│   │   ├── 2025/
│   │   │   └── qemu-camp-2025-LordaeronESZ.md
│   │   ├── 2026/
│   │   │   ├── qemu-camp-2026-rd.md      # RD 开发板实验
│   │   │   └── qemu-camp-2026-dingtao1.md # 鼎道 1 号实验
│   │   └── misc/
│   │       ├── qemu-cnb-dev.md            # CNBridge 开发
│   │       ├── simulater-interp.md        # 模拟器解释
│   │       └── qemu-devel-email.md       # 开发邮件指南
│   └── weekly/                   # 周报 (进展同步)
│       └── 2026/
│           └── qemu-riscv-weekly-2026-03-05.md
├── mkdocs.yml                    # 网站配置
└── README.md
```

### 文档使用指南

**按学习顺序阅读**：

1. **导学阶段** (`stage0`) - 语言基础
   - `learning-c.md` / `learning-rust.md`
   - 补充 C 语言或 Rust 语言知识

2. **基础理论** (`tutorial/ch0-ch2`) - 核心概念
   - `ch0/`: 环境搭建、学习资源
   - `ch1/`: QOM、MemoryRegion、初始化流程
   - `ch2/`: TCG、设备模型、中断系统

3. **实验要求** (`exercise/stage1`) - 动手实践
   - 先读 `g233-exper-manual.md` 了解测试要求
   - 再查 `g233-datasheet.md` 查找寄存器规格

4. **经验参考** (`blogs`) - 实战经验
   - 阅读往年学员的实验记录
   - 学习具体问题的解决方法

### SoC 实验文档使用详解

**实验手册** (`g233-exper-manual.md`)

包含内容：
- **环境搭建**：依赖安装、仓库获取、编译配置
- **提交代码**：如何提交到 GitHub 和 CI 验收
- **测评验收**：本地测试命令、评分机制
- **实验介绍**：10 个实验的测试用例说明

关键章节结构：
```markdown
### 实验一 test-board-g233
| 源码路径 | tests/gevico/qtest/test-board-g233.c |
| 功能描述 | 验证 G233 Board 基本工作 |
| 详细规格 | G233 SoC 硬件手册 §3-§5 |
| 基础代码 | hw/riscv/g233.c（需补全）|

| 测试用例 | 测试内容 |
|---------|---------|
| test_board_init | 机器启动测试 |
| test_dram_access | DRAM 读写测试 |
| test_plic_present | PLIC 寄存器测试 |
| test_clint_present | CLINT 寄存器测试 |
```

**硬件规格** (`g233-datasheet.md`)

包含内容：
- **系统架构**：顶层框图、外设互联、中断源分配
- **地址映射**：每个设备的基地址和大小
- **寄存器定义**：每个外设的完整寄存器表、位域定义
- **编程模型**：设备的使用方法和注意事项

关键章节结构：
```markdown
## 7. GPIO 控制器

### 7.1 寄存器映射
GPIO 基地址：0x1001_2000

| Offset | 寄存器 | 访问 | 复位值 | 描述 |
|--------|--------|------|--------|------|
| 0x00   | GPIO_DIR | R/W  | 0x00000000 | 方向寄存器 |
| 0x04   | GPIO_OUT | R/W  | 0x00000000 | 输出数据 |
| 0x08   | GPIO_IN  | R    | 0x00000000 | 输入数据 |

### 7.2 寄存器定义

#### 7.2.1 GPIO_DIR (Offset 0x00)
| Bit | 名称 | 访问 | 复位 | 描述 |
|-----|------|------|------|------|
| 31:0 | DIR | R/W  | 0x00000000 | 方向位 (0=输入, 1=输出) |
```

### 基础理论学习路径

**设备建模入门** (tutorial/ch1):

1. `qemu-qom.md` - 理解 QEMU 对象系统
   - 类型注册、继承、属性
   - TYPE_DEVICE、SysBusDevice 的关系

2. `qemu-mr.md` - 理解内存区域抽象
   - MemoryRegion 层次结构
   - MMIO、RAM、Alias 的区别

3. `qemu-init.md` - 理解设备初始化流程
   - realize()、reset() 的调用时机
   - sysbus_create_simple() 的作用

**深入核心机制** (tutorial/ch2):

1. `qemu-hw.md` - 设备模型实现模式
   - MemoryRegionOps、设备状态结构
   - 中断线连接

2. `qemu-intr.md` - 中断系统
   - PLIC、ACLINT 的工作原理
   - qemu_irq 的使用

3. `qemu-tcg.md` - TCG 指令翻译
   - TCG 前端、后端、中间表示
   - 如何添加自定义指令

### 实验开发完整流程

```
1. 阅读 tutorial/ch1/ 的基础概念
   ↓ 理解 QOM、MemoryRegion
2. 查看 g233-exper-manual.md
   ↓ 了解实验要求
3. 查看测试代码 tests/gevico/qtest/test-xxx.c
   ↓ 理解期望行为
4. 参考 g233-datasheet.md
   ↓ 查找寄存器定义
5. 实现 hw/xxx/g233_xxx.c
   ↓ 编写设备模型
6. 运行测试验证
   ↓ make -f Makefile.camp test-soc
```

## 架构

### G233 机器

G233 虚拟机 (`hw/riscv/g233.c`, 77KB) 扩展标准 `virt` 机器。核心组件：

- **CPU**: riscv.g233.cpu (RV64GC RVA23 + Xg233ai 扩展)
- **DRAM**: 1 GB @ `0x8000_0000`
- **ACLINT**: 定时器/软件中断 @ `0x0200_0000`
- **PLIC**: 平台级中断控制器 @ `0x0C00_0000`
- **MROM**: 启动 ROM @ `0x0000_1000`

### SoC 设备模型

所有 SoC 外设都是内存映射设备，通过 MMIO 访问：

| 设备 | 基地址 | PLIC IRQ | 实现 |
|------|--------|----------|------|
| PL011 UART | `0x1000_0000` | 1 | `hw/char/pl011.c` |
| WDT | `0x1001_0000` | 4 | 待实现 |
| GPIO | `0x1001_2000` | 2 | `hw/gpio/g233_gpio.c` |
| PWM | `0x1001_5000` | 3 | 待实现 |
| SPI | `0x1001_8000` | 5 | 待实现 |

### 设备模型模式

所有 SoC 设备遵循 QOM 模式：

1. **状态结构**: `typedef struct G233XXXState { SysBusDevice parent_obj; MemoryRegion iomem; qemu_irq irq; uint32_t registers... }`
2. **读/写回调**: `static uint64_t device_read(void *opaque, hwaddr offset, unsigned size)` 和 `static void device_write(...)`
3. **MemoryRegionOps**: `.read = device_read, .write = device_write, .endianness = DEVICE_LITTLE_ENDIAN`
4. **Realize**: `memory_region_init_io(&s->iomem, OBJECT(s), &device_ops, s, TYPE_DEVICE, size)`
5. **注册**: `sysbus_init_mmio(sbd, &s->iomem); sysbus_init_irq(sbd, &s->irq)`

### QTest 框架

测试使用 QTest 协议直接访问 MMIO，无需运行客户机 OS：

```c
QTestState *qts = qtest_init("-machine g233");
qtest_writel(qts, 0x10012000, 0x1);           // 写 MMIO
uint32_t val = qtest_readl(qts, 0x10012000);  // 读 MMIO
qtest_quit(qts);
```

QTest 调用 QEMU 的 `address_space_write()`，路由到设备的读/写回调。

### 中断

设备通过 `qemu_set_irq(s->irq, 1)` 触发中断，路由到 PLIC 输入 1-5。PLIC pending 位是 sticky 的，软件必须通过 claim/complete 清除。

## 内存映射参考

G233 关键地址（完整规格见 `docs/tutorial/docs/exercise/2026/stage1/soc/g233-datasheet.md`）：

- `0x8000_0000`: DRAM 起始
- `0x0C00_0000`: PLIC 基地址
- `0x0200_0000`: ACLINT 基地址
- `0x1001_2000`: GPIO 基地址
- 参见硬件手册获取完整设备寄存器偏移

## 重要提示

- **不要修改测试源码** `tests/gevico/qtest/` - 只实现设备模型
- **所有 MMIO 寄存器都是 32 位**（除非另有说明）
- **Write-1-to-clear**：许多状态寄存器（如 GPIO_IS）使用此模式：`s->is &= ~value`
- **QEMU 代码风格**：遵循 `hw/` 中现有模式保持一致性
- **PLIC IRQ 映射**：UART=1, GPIO=2, PWM=3, WDT=4, SPI=5

## 依赖

- CPU 实验需要 RISC-V 交叉编译器：`riscv64-unknown-elf-gcc`
- Rust 实验及构建需要 Rust 工具链：`cargo install bindgen-cli`
- QEMU 构建依赖：`sudo apt-get build-dep qemu`
