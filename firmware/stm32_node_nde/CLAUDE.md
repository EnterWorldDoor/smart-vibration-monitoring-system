# STM32 NDE Node — Claude Code Configuration

## Project Identity
- **MCU**: STM32F103C8T6 (Cortex-M3, 72MHz, 20KB SRAM, 64KB Flash)
- **Board**: Blue Pill 最小系统板 (LQFP48)
- **IDE**: STM32CubeIDE 1.15+ with STM32CubeMX
- **Architecture**: Bare metal (no RTOS)
- **Sensor**: ADXL345 三轴加速度计 (SPI, 400Hz)
- **CAN**: SN65HVD230 收发器, 500kbps, 标准11-bit ID
- **Compiler**: arm-none-eabi-gcc, `-mfloat-abi=soft` (no FPU)

## STM32CubeMX Protection

CubeMX regenerates Core/Src/main.c and Core/Src/stm32f1xx_it.c outside USER CODE guards.
The .ioc file is the single source of truth for peripheral configuration.

**Rule**: After every CubeMX regeneration, verify:
1. CAN Remap enabled (PB8/PB9) — CAN_InitStruct.Mode check
2. TIM2 period = 9, prescaler = 17999 (400Hz)
3. SPI1 master mode, 8-bit, prescaler ≤ 16 (≤4.5MHz)

## Build System
1. Open `stm32_node_nde.ioc` in CubeMX
2. Generate code (Project → Generate Code)
3. Build in CubeIDE: Project → Build Project
4. Flash via ST-LINK or serial bootloader

## Key File Map

```
Core/Src/main.c              — Boot sequence (CubeMX generated)
App/app_main.c               — Bare metal main loop (USER CODE WHILE)
App/adxl345_drv.c            — ADXL345 SPI driver
App/dsp_nde.c                — Feature extraction (24-dim, calls dsp_fft_q15)
App/dsp_fft_q15.c            — Self-contained 64-pt radix-2 real FFT (no CMSIS-DSP)
App/dsp_fft_q15.h            — FFT interface (q15 types, 33 complex bins)
App/can_send.c               — CAN multi-frame sender (0x201 + 0x202)
```

## Coding Conventions
- **Style**: Linux kernel (8-char indent, lowercase_underscore)
- **Error handling**: Return negative errno (-1, -2)
- **NO dynamic malloc** — 20KB SRAM, all static allocation
- **NO float in printf** — newlib-nano doesn't support %f
- **NO CMSIS-DSP** — Self-contained dsp_fft_q15 (~2KB Flash) replaces CMSIS-DSP
  which would consume ~62KB Flash (RFFT tables alone overflow the 64KB budget)

## Pin Assignments
| Peripheral | Pin | Function |
|-----------|-----|----------|
| ADXL345 SCLK | PA5 | SPI1_SCK |
| ADXL345 MOSI | PA7 | SPI1_MOSI |
| ADXL345 MISO | PA6 | SPI1_MISO |
| ADXL345 CS | PA4 | Software GPIO |
| ADXL345 INT1 | PA3 | External interrupt |
| CAN RX | PB8 | CAN_RX (remap) |
| CAN TX | PB9 | CAN_TX (remap) |
| USART1 TX | PA9 | Debug serial |
| USART1 RX | PA10 | Debug serial |
| USB DM/DP | PA11/PA12 | Micro-USB power + serial |

## Task Architecture (Bare Metal)
```
main():
  init peripherals → init ADXL345 → init DSP → init CAN
  while(1):
    TIM2 400Hz flag → SPI FIFO read (64 samples)
    → DSP: FFT + RMS + 24-dim features
    → Every 2s: CAN send 16-frame batch + heartbeat
```

## Hardware Notes
- CAN requires remap: `__HAL_AFIO_REMAP_CAN1_0_ENABLE()` or equivalent
- F103 CAN APB1 clock = 36MHz (APB1 prescaler /2 of 72MHz)
- CAN timing for 500kbps: Prescaler=3, BS1=CAN_BS1_11TQ, BS2=CAN_BS2_2TQ
- USART1 baud = 115200, APB2=72MHz → USARTDIV = 39.0625
