# 协作约定

整个 TArgetCarrier 使用一个 Git 仓库；每人克隆到自己电脑并独立构建。
main 保存已确认版本，每项任务新建功能分支，完成后发起 Pull Request，由另一位协作者检查后合入。
分支示例：feat/xaxis-freertos、feat/carrier-control。

两个固件分别使用自己的 CMake 入口和交叉工具链。统一工具链版本后再记录为项目要求；当前尚未锁定版本，也未配置自动构建。
不提交构建缓存、个人环境路径和无线配对密钥。sdkconfig/defaults 是共享配置，机器或配对参数应单独保存在被忽略的本地文件。
COPY_MANIFEST.csv 仅为本机初次复制校验记录，不上传。

STM32 已移植 FreeRTOS-Kernel V11.2.0；ESP32-S3 已使用 FreeRTOS。STM32任务、LED宏配置和构建方式见 x轴/野火stm/FREERTOS_PORT.md。
修改 .ioc 前先约定负责人；双方通信协议变化需要一起评审。
每次烧录记录 Git 提交号、配置和测试结果。构建成功不能代表硬件联调通过。
靶车/.gitkeep 仅用于保留空目录，靶车固件尚未开始。

本次上传仅核验复制文件一致性与远端提交，未重新编译、烧录或进行硬件验证。
