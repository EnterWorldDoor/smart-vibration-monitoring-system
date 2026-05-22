# STM32 NDE节点 — AI开发智能体规范 (AGENTS.md)
**项目**: NDE振动传感器节点
**MCU**: STM32F103C8T6 (Cortex-M3, 72MHz, 20KB SRAM, 64KB Flash)
**IDE**: STM32CubeIDE 1.15+
**架构**: 裸机 (无RTOS)
**编码标准**: **Linux Kernel 编码规范** + 工业级嵌入式最佳实践

---

## 编码规范 (完整继承 F407 AGENTS.md)

详见 `../stm32_node_vibration/AGENTS.md` 第2-9节。要点速查:

- **缩进**: 8空格, 禁止Tab
- **注释**: `/* */`, 禁止 `//`
- **命名**: 小写下划线, 函数=`模块名_功能名`, 宏=全大写
- **结构体**: 禁止typedef, 函数指针可用`_t`后缀
- **错误处理**: 返回负errno (`-EINVAL`, `-EIO`, `-ETIMEDOUT`), goto统一清理
- **变量**: 函数内变量在开头声明
- **禁止**: 动态内存、全局变量、忽略返回值、`%f`格式化、魔法数字

## F103 特有硬约束

- **无FPU**: Cortex-M3, `-mfloat-abi=soft`, 浮点运算全软仿真
- **无RTOS**: 裸机主循环, 谨慎处理阻塞时间
- **无CMSIS-DSP**: 常量表超Flash(62KB), 使用自写 `dsp_fft_q15`
- **SRAM 20KB**: 预算 ~3KB (15%), 全静态分配
- **Flash 64KB**: 预算 ~35KB (55%), HAL + CMSIS + 应用

## 分层架构

```
App/          → 纯应用逻辑, 禁止调HAL
bsp/          → 硬件驱动封装, 唯一可调HAL的层
Core/         → CubeMX生成, USER CODE区域可修改
Drivers/      → ST官方HAL, 只读
```

## CubeMX 保护规则

| 文件 | 保护区域 | 风险 |
|------|---------|------|
| `Core/Src/main.c` | `USER CODE BEGIN Init/WHILE` | MX_IWDG_Init需手动添加 |
| `Core/Src/stm32f1xx_it.c` | `USER CODE BEGIN` | EXTI3_IRQHandler |
| `Core/Src/gpio.c` | `USER CODE BEGIN` | PA3 EXTI配置 |

**规则**: CubeMX重新生成后检查:
1. CAN重映射(PB8/PB9)保留
2. PA3 GPIO模式(改为中断模式后需验证)
3. USART1调试串口保留

## 文件地图

```
App/app_main.c          — 裸机主循环(FIFO采样+DSP+CAN+IWDG)
App/app_main.h          — 健康状态枚举 + 接口声明
App/dsp_nde.c/h         — 24维特征提取(纯算法)
App/dsp_fft_q15.c/h     — 64点radix-2 RFFT(自写, 无CMSIS-DSP)
App/can_send.c/h        — CAN协议层(CRC8组帧, 调用bsp_can)
bsp/adxl345/            — ADXL345 SPI驱动 + FIFO突发 + INT1 EXTI
bsp/can/                — CAN HAL封装 + CRC8计算
bsp/bsp_log.h           — 精简日志宏
Core/Src/main.c         — 启动序列(CubeMX生成 + USER CODE)
Core/Src/stm32f1xx_it.c — 中断向量(CubeMX生成 + EXTI3 handler)
```
