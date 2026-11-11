# 后端、接线与资源

参考对象来自副本：ESP32-S3-GEEK总机、S3无线接收工程、野火STM32F103C8T6双USB的CMSIS/启动与64 KiB Flash /20 KiB RAM链接脚本。未将未确定的后续步兵芯片假定为F103。引脚表是本次独立参考固件的配置，尚未接线实测。

所有信号3.3 V、共地；不把5 V接到ESP32 GPIO。ESP32和STM32各自稳定供电，接线前断电。本次未进行这些操作。

| 后端 | ESP32-S3接收端 | STM32F103C8T6 | 方向/说明 |
|---|---|---|---|
| UART | GPIO4 TX | PB11 USART3_RX | ESP→STM |
| UART | GPIO5 RX | PB10 USART3_TX | STM→ESP |
| SPI | GPIO12 SCK | PA5 SPI1_SCK | ESP主机→STM从机 |
| SPI | GPIO11 MOSI | PA7 SPI1_MOSI | ESP→STM |
| SPI | GPIO13 MISO | PA6 SPI1_MISO | STM→ESP |
| SPI | GPIO10 CS | PA4 SPI1_NSS | ESP输出，低有效 |
| SPI | GPIO9 READY输入 | PB1 READY输出 | STM DMA已布置，允许一次事务；高时不必一定有业务数据 |
| I²C | GPIO18 SCL | PB10 I2C2_SCL | ESP主机，地址0x42 |
| I²C | GPIO8 SDA | PB11 I2C2_SDA | 双向开漏；板外配置上拉至3.3 V |
| I²C | GPIO9 DRDY输入 | PB1 DRDY输出 | 应答邮箱待取为高，空为低 |
| 全部 | GND | GND | 公共参考地 |

I²C上拉建议先用4.7 kΩ、短线、100 kHz，再按实际总线电容与示波器上升沿调整；不是已测定的板载上拉。STM32 PB1在独立程序中作为输出，接入新项目前必须确认未被ADC/定时器或其他外设占用。

## UART（默认建议）

460800、8N1、无硬件RTS/CTS；ESP-IDF UART1 RX/TX驱动缓冲各1024 B。STM USART3 RX=DMA1通道3、512 B循环，TC中断累计圈数；主任务按DMA写位置消费并滑窗解析。USART3 TX=DMA1通道2，每次一个128 B槽，DMA完成前不改缓冲。数据量过大造成环形缓冲超过512 B时丢弃旧字节并重置解析器，不能把积压命令当新命令。

UART无总线级硬背压。可靠请求使用停等/超时，波形使用限流/有界丢弃。约11.1 ms填满512 B RX，因此协议任务推荐至少1 ms调度一次、保持中断可运行；不能在同优先级里长时间屏蔽中断。需要更强硬背压时替换板级适配增加CTS/RTS，不能仅扩大无限队列。

## SPI

ESP32 SPI2_HOST主机、STM32 SPI1硬件NSS从机，Mode0、8bit、MSB first，建议起步1 MHz、每次128 B全双工，ESP DMA静态缓冲、STM DMA1通道2 RX/通道3 TX。没有数据的方向发全零槽（不是有效TC帧）。

ESP先检查READY，再从队列取报文；最长每10 ms调度一个事务。STM在NSS上升沿EXTI4中断中检查DMA长度/溢出，收完恰好128 B才提交RX，短事务保留TX并重新布置DMA。下一次事务的TX在前一个事务结束时预装，故一次请求的应答需后续轮询，不能假设同一CS内即时返回。

READY表示已布置DMA，非“当前必有应答”。ESP空轮询可推动STM发布下一应答。主机必须保持事务串行、CS高间隔足够让STM结束中断并重新拉高READY；本版10 ms轮询是保守参考值，不是已验证的最小时序。不能让另一SPI主机无视READY任意加速。PA4还需EXI4资源；不要同时注册其他EXTI4处理器。

## I²C

ESP I2C0主机、STM I2C2从机，7位地址0x42，建议100 kHz；STM PB10/PB11开漏，EV/ER中断优先级5，不使用DMA。ESP每10 ms至多发一个待发写事务并取一个DRDY应答。单次API超时25 ms；接收端50 ms事务超时重置I2C外设计数和状态。外部器件拉死总线不一定能靠复位MCU外设解除，需硬件验收及项目恢复策略。

请求以128 B独立WRITE+STOP提交，不用“串口读取N字节”。应答是128 B独立READ；DRDY为空时不盲读。STM保留发布的应答邮箱，NACK只结束读，不释放邮箱。ESP CRC通过后，再WRITE一个12 B确认：`'A','K',2,0, seq:u32, slot_crc32:u32`。STM校验匹配当前邮箱才释放。短读、丢确认会重复读到同一个结果，协议层按序号去重。READY长期高且重复读应在诊断中定位，不当作新状态。

I²C写请求无寄存器地址前缀。若未来桥接到寄存器型器件，需要替换事务适配层，不应改变上层参数协议。中断中只搬运字节/槽和计数；CRC和应用在任务中运行（当前接收ISR在事务结束做单槽CRC，128 B固定上限；上板应测其耗时，若不满足项目预算可改为仅标记待校验）。

## 已核对的冲突与边界

- PB10/PB11：USART3与I2C2复用，同一实例不能并行。
- DMA1通道2/3：USART3 TX/RX与SPI1 RX/TX重叠，本参考用编译互斥消除冲突。
- 旧x轴PA8/PB0用于PUL/DIR，旧OLED常用PB6/PB7；新独立程序均未初始化这些业务。不能因“没有直接同针”就把新main直接覆盖进电机工程。
- 旧GEEK屏幕占用SPI2与GPIO7/8/9/10/11/12。本包GEEK角色只初始化USB和无线，**不初始化有线后端**；接线表只适用于普通S3接收端，不可直接在GEEK上与屏幕同时启用。
- 普通S3接收板的具体载板外接资源未由实物确认；GPIO8/9/10/11/12/13/18是建议板级配置，须核对实际排针、OLED、Flash/PSRAM及其他连接。没有据此声称任意S3板均可直连。
- 当前独立STM程序采用HSE8 MHz→72 MHz，PCLK1=36 MHz，PCLK2=72 MHz，独立SysTick 1 ms。原FreeRTOS工程的中断、时钟和任务不被替换。接入RTOS时使用其时基，去掉示例SysTick，按PORTING.md合并IRQ。

官方依据：[ESP-IDF SPI Master](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_master.html)、[ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_now.html)、[ST STM32F103x8/xB 数据手册](https://www.st.com/resource/en/datasheet/stm32f103t8.pdf)。引脚/IRQ/DMA同时以副本实际CMSIS头文件和已有board配置交叉核对，参考资料不能替代载板实测。
