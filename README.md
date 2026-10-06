# SMIT 融合电视伴侣

为 **融合电视伴侣（iCast）USB 电视接收器**提供实验性的 Linux 驱动、HTTP 出流服务和 Python 控制／分析工具。项目基于 [上游 iCast 项目](https://github.com/nxdong520/iCast) 改造，使用系统的 `dvb-usb-v2` 和标准 Linux DVB 接口，不需要运行原厂 Android 应用。

内核驱动负责调谐、接收 MPEG-TS 和转发条件接收（CA）消息；用户态服务负责节目选择和原始流输出；客户端负责控制、录制及离线分析。对于有合法授权的加扰节目，解扰由设备和智能卡完成。

## 硬件与适配范围

目前适配和测试的是**融合电视伴侣多模版本**，驱动匹配的 USB VID:PID 为 `29df:0001`。**单模版本未经过测试**，不能据相同名称或 USB ID 推断兼容性。

| 项目 | 当前范围 |
| --- | --- |
| 接收制式 | 驱动仅开放 DVB-C Annex A |
| 调制方式 | 当前实现并实测 QAM64；其他 QAM 未开放、未验证 |
| DTMB | 多模设备具备相关能力声明，但当前驱动未实现、未实测 |
| 主机 | Ubuntu x86_64；OpenWrt ramips/mt7621 有构建和有限硬件验证记录 |
| 条件接收 | 支持授权智能卡的 CA 消息交互；兼容性取决于卡、运营商和固件 |

上述结果不代表全频段、全部符号率、所有硬件修订或长期稳定性已经验证。

## 已实现功能

以下 `[x]` 表示当前代码具备该功能，支持范围与限制以上表和技术文档为准。

- [x] USB 设备识别、控制通道初始化和 CI/SAS 动态会话管理。
- [x] 使用标准 `frontend0`、`demux0`、`dvr0` 接口接入 Linux DVB 子系统。
- [x] DVB-C Annex A / QAM64 调谐、锁定状态与信号强度查询。
- [x] 实验性高层 `ca0` 接口，支持 CA_INFO、CA_PMT 及消息队列。
- [x] 从 PAT/PMT 读取节目信息，按节目生成并提交 CA_PMT。
- [x] HTTP JSON API：调谐、状态、节目选择、节表读取、CA 控制及出流统计。
- [x] 单客户端原始全复用 MPEG-TS 出流，不改写广播 PSI/SI 或进行转码。
- [x] Python 3 客户端控制、定时录制和有界内存流监测，可运行于 Windows、macOS、Linux。
- [x] TS 连续性、TEI、加扰位及 CA/USB 抓包离线分析工具。
- [x] Ubuntu 内核模块／服务端构建与 OpenWrt APK 打包、procd 服务配置。
- [x] 协议、CA 队列、PSI、HTTP 服务和客户端等离线回归测试。

## 构建

使用 Linux 构建主机；Windows 可通过 WSL 编译。准备 GNU Make、Python 3、C 编译器和目标平台工具链。内核模块需要与目标内核匹配的 headers、`Module.symvers`，以及对应内核源码中的 `dvb-usb-v2` 私有头文件。

下列命令中的 `<repository-url>` 为仓库地址，`/path/to/...` 为需要替换的路径占位符。

### Ubuntu / x86_64

```sh
git clone <repository-url> smit
cd smit

# CC 需与目标内核构建环境匹配；以下为现有 Ubuntu 构建示例。
make -j4 PLATFORM=ubuntu-x86_64 CC=gcc-15 \
  KDIR=/lib/modules/$(uname -r)/build \
  DVB_USB_V2_DIR=/path/to/matching-linux-source/drivers/media/usb/dvb-usb-v2
```

产物位于 `build/ubuntu-x86_64/`，包括 `smit.ko`、`smit` 服务端及 SHA256 清单。已有驱动时可单独执行 `make server`；仅构建模块可执行 `make module` 并传入上述内核路径参数。

### OpenWrt / mt7621

当前构建脚本针对 OpenWrt 25.12.5 / ramips/mt7621 和 Linux 6.12.94 资源布局；不能直接套用于任意 OpenWrt 版本。

```sh
make PLATFORM=openwrt-mt7621 SDK=/path/to/matching-sdk \
  RESOURCE_CACHE=/path/to/verified-resources JOBS=4
```

`RESOURCE_CACHE` 需提供脚本要求的 `linux-6.12.94.tar.xz`。SDK 必须已准备好匹配的内核构建目录、`.config`、`Module.symvers` 和生成头文件；脚本会校验固定哈希，单纯下载并解压 SDK 不保证满足条件。SDK 路径不能包含空格，输出目录应避免混入旧版 APK。

产物位于 `build/openwrt-mt7621/`，包含 `smit.ko`、`smit` 和 APK 包；安装时一并提供所需依赖，并保持内核版本与 ABI 一致，勿强行跳过依赖检查。资源要求和校验见 [构建脚本](tools/build/openwrt.sh)。

## 使用方法

### 1. 连接设备并启动服务

接入当地有线电视 RF 信号。需要解扰时，建议在设备上电前插入具有相应节目授权的智能卡；无卡启动后再插卡的恢复尚不可靠。

```sh
sudo modprobe dvb_usb_v2
sudo insmod build/ubuntu-x86_64/smit.ko
ls /dev/dvb/adapter0/

# 运行用户须具有相应 DVB 设备节点的访问权限。
./build/ubuntu-x86_64/smit --bind 127.0.0.1 --port 38212 \
  --adapter /dev/dvb/adapter0
```

若设备不是 `adapter0`，替换为实际路径。OpenWrt 安装对应 APK 后使用 `/etc/init.d/smit start|stop|restart` 管理服务，配置位于 `/etc/config/smit`；默认绑定 `127.0.0.1`。

### 2. 调谐并选择节目

另开终端执行以下命令。示例使用 POSIX shell 引号语法；PowerShell 传递 JSON 时需遵循所用版本的原生命令参数规则。

先将 shell 变量 `FREQUENCY_HZ`、`SYMBOL_RATE` 和 `SERVICE_ID` 设为接收网络提供的频率、符号率与目标节目号。它们应为有效的十进制整数；这里不提供任何实际广播网络参数。

```sh
python3 tools/smit.py call /health
python3 tools/smit.py call /frontend/tune "$(printf '{"frequency":%s,"symbol_rate":%s,"modulation":64}' "$FREQUENCY_HZ" "$SYMBOL_RATE")"
python3 tools/smit.py call /frontend/status '{}'

# 等到 locked:true 后，按实际 PAT/PMT 中的节目号选择节目。
python3 tools/smit.py call /service/select "$(printf '{"service":%s}' "$SERVICE_ID")"
```

频率单位为 **Hz**，符号率单位为 **symbols/s**。项目没有自动搜台或频道库入口。选节目响应中的 `streams` 列出当前 ES PID，`ca_submitted:true` 只表示已提交 CA_PMT，不能作为成功解扰或授权证明。

### 3. 录制、监测或播放

```sh
# 原始流录制到客户端；输出文件必须尚不存在。
python3 tools/smit.py stream --seconds 30 --output capture.ts --report capture.json

# 不保存 TS，只监测；VIDEO_PID、AUDIO_PID 须设为选节目响应中的实际目标 PID。
python3 tools/smit.py stream --seconds 30 --pid "$VIDEO_PID" --pid "$AUDIO_PID" --report monitor.json

# 停止活动出流后，才能重新调谐、选节目或复位。
python3 tools/smit.py call /stream/stop '{}'
```

播放器也可打开 `http://127.0.0.1:38212/stream`，并在播放器内选择所需节目。一次只允许一个流消费者；录制与播放器同时连接会产生冲突。

访问远端服务时，在 `call`／`stream` 前加 `--url http://<服务端地址>:38212`，并明确配置服务端绑定地址。API 没有认证或 TLS，只应在受控的可信网络使用，勿直接暴露到公网。

### 4. 离线分析和测试

```sh
python3 tools/smit.py analyze-ts capture.ts > tables.json
python3 tools/smit.py analyze-ca capture.ts tables.json > ca-analysis.json
python3 tools/smit.py usbmon-pcap --help
python3 tools/smit.py inspect-sas --help
python3 tools/smit.py --help

# 在 Linux 构建主机执行离线回归，不需要接入设备。
make check
```

## TODO

- [ ] 在单模版本、其他多模硬件修订和固件上验证兼容性。
- [ ] 研究并实现 DTMB，验证其他 QAM 调制与更多频点／符号率组合。
- [ ] 完善无卡上电后插卡、USB 异常断开、系统休眠／恢复等生命周期验证。
- [ ] 验证智能卡事件、CA_PMT_REPLY 和真实广播 PMT 变化的完整处理链。
- [ ] 验证长 CA_PMT 发送，明确大 PMT／多描述符节目的支持边界。
- [ ] 增加长期收流、反复拔插、KASAN／lockdep 等内核压力测试。
- [ ] 增加用户态自动搜台、频道库和明确的故障恢复流程。
- [ ] 按需求实现完整单节目重复用、多客户端分发与安全访问控制。

## 注意事项

- `/stream` 是 `dvr0` 原始全复用流。其他节目可能仍加扰，且 PAT/PMT/SDT 的 CA 标记保持原样；播放器显示“加密”不一定说明所选音视频仍加扰。
- 服务端不做清流门控或自动重调谐。刚选台或刚连接时可能出现短暂加扰、连续性异常；断流后须检查锁定和卡状态，必要时停止出流后重新调谐／选台。
- 客户端 `clear_verified` 仅代表指定 PID 在观察窗口中的 TS 加扰位／清流载荷检查，不等于完整音视频解码验证。未提供 `--pid` 时不会给出目标节目清流确认。
- 固件名为 `snr` 的字段实际用于相对信号强度，不能当作信噪比；当前不报告可信 CNR 或 RF BER。TS 的 TEI／CC 错误也不是 RF BER。
- `ca0` 为实验性高层 `CA_CI` 接口，非 `CA_CI_LINK`；第三方 DVB 应用仍需验证 CA 兼容性。卡状态、CAID 和 slot ready 都不证明节目授权。
- 长控制帧发送、设备电源命令及 EP0 恢复等实验路径默认关闭；不要把恢复代码的存在当作恢复已可靠。升级协议仅有研究记录，未提供固件升级功能。
- 模块必须匹配目标内核。停止服务和其他 DVB 使用程序后再卸载驱动，避免在设备仍被占用时强行卸载。

## 技术细节文档

- [USB / CI / SAS 协议](docs/device-protocol.md)：报文格式、会话、命令、调谐参数、信号和 CA 边界。
- [驱动与服务架构](docs/architecture.md)：模块职责、DVB 接口、控制串行化及生命周期。
- [HTTP API 与工具](docs/http-api.md)：路由、JSON 参数、流行为、错误和诊断入口。

## 免责声明

本项目是独立的实验性开发与协议研究，与设备厂商、原厂应用或广播运营商没有隶属关系，也不代表其官方支持。协议描述来自已有开源实现和协议分析，可能不适用于其他版本。

请仅使用自己有权接收的信号和已获得合法授权的节目；本项目不提供绕过订阅授权、提取密钥或破解条件接收的功能。使用时应遵守所在地适用规定和服务条款。实验驱动可能导致接收中断、系统异常或数据丢失；请自行评估运行环境并做好备份，项目不承诺可用性、完整兼容性或持续稳定运行。

## 参考与致谢

感谢 [上游 iCast 项目](https://github.com/nxdong520/iCast)，为设备接入和协议研究提供了基础；感谢 Linux media/DVB 维护者，以及提供资料、设备和测试反馈的参与者。源文件中的原作者与许可证声明应予保留；引用第三方代码和资料时应遵守其许可证。

技术接口可参考 [Linux DVB API](https://docs.kernel.org/userspace-api/media/dvb/dvbapi.html)、[高层 CI API](https://docs.kernel.org/userspace-api/media/dvb/ca_high_level.html) 和 [USB monitoring（usbmon）](https://docs.kernel.org/usb/usbmon.html)。上游 iCast 的能力描述不等同于本项目当前实现或测试范围。
