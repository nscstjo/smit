# 融合电视伴侣 USB / CI / SAS 协议

本文说明当前 `driver/` 使用的传输格式、会话模型和命令语义，适用范围为融合电视伴侣多模版本（USB ID `29df:0001`）；单模版本未测试。本文不是厂商协议规范。命令目录区分当前实现与尚未开放的研究项，不能据此推断所有硬件或固件支持。

## 1. USB 通道与数据路径

| 项目 | 当前值／行为 |
| --- | --- |
| USB 接口 | 0 |
| 控制发送 | bulk OUT `0x01` |
| 控制接收 | bulk IN `0x82` |
| TS 接收 | bulk IN `0x84` |
| 流缓冲 | `dvb-usb-v2` 配置 8 × 8192 字节 |
| 数据格式 | 188 字节 MPEG-TS 包；USB 传输边界不是 TS 包边界 |

端点 `0x82` 与下面传输请求 `0x82` 属于不同层次；不要混用端点、传输请求、资源 ID、会话 ID、APDU tag 和 SAS 内部命令号。

```text
USB 01/82 → 传输帧 → CI 资源／会话 → APDU → SAS 内部命令
USB 84    → MPEG-TS → dvb-usb-v2 → 软件 demux → dvr0
```

GDHW 返回硬件能力文本，GTYP 返回设备类型文本。字段与取值可能因硬件或固件不同而变化，不应将完整回复作为固定协议常量，也不能据能力声明推断当前驱动已实现相应功能。GTYP 子字段语义尚未完整确认。

## 2. 冷握手与外层帧

冷握手请求为 `fe 00 10 00`，成功回复为 `ff 00 20 00`。已初始化设备可能不再回应冷握手，不能把它当作通用热复位。

普通帧采用以下格式（字节以十六进制表示）：

```text
01 00 request length payload          # payload 0..127 字节
01 00 request 81 length payload       # payload 128..255 字节
```

当前外层 payload 上限为 255 字节。`81 xx` 为 BER 一字节长形式；不支持 `82` 两字节长度或任意分片重组。接收解析必须处理短长度和已支持的长长度形式。

| 传输请求 | 当前用途 |
| --- | --- |
| `82` | 建立传输 |
| `81` | fetch 待取数据 |
| `a0` | 发送消息／轮询；轮询 payload 为 `01` |

`80 02 01 00/80` 等传输状态标志用于判断是否有待取数据，不证明内部 SAS 命令执行成功。长度及完成标志检查见 [传输编解码](../driver/smit_transport_codec.h)，USB 收发见 [传输实现](../driver/smit_transport.c)。

接收解析支持上述长长度形式，长 OUT 的可靠性仍待验证。当前默认拒绝外层 payload ≥128 的发送；实验性 `experimental_long_tx` 可放宽限制。APDU 前还有 5 字节会话包装，因此**默认发送 APDU 最大为 122 字节**，实验性上限为 250 字节。大 PMT 必须显式报错，不能通过截断 CA 描述符伪造成功。

## 3. CI 资源和动态会话

设备使用 `91 04 <resource>` 请求资源，主机用 `92 07 <status> <resource> <session>` 应答，位于外层传输 payload 的 CI 包装内。会话 ID 按大端存储。

| 资源 | ID | 当前用途 |
| --- | --- | --- |
| Resource manager | `00010041` | profile 交换 |
| Application information | `00020041` | application enquiry |
| Conditional access | `00030041` | CA_INFO、CA_PMT 和回复 |
| SAS | `00961001` | 调谐、设备信息和通知 |

SAS connect APDU 为 `9f9a00`，当前实现的正文为 `SMiTZBJL`；成功应答 `9f9a01` 携带身份及状态 `00`。正文属于协议连接标识，不是设备序列号或个人标识；不同固件的兼容性需单独验证。

当前最多维护 8 个资源会话，动态分配会话 ID。会话 ID 不应写死，资源类型与会话必须正确关联，尤其不能将 CA 消息发送到 SAS 会话。主 SAS ready 是初始化前提；CA_INFO 或智能卡存在不是 frontend 注册前提。

## 4. SAS APDU 编码

会话包装：`01 90 02 <session BE16> <APDU>`。SAS APDU tag 为 `9f 9a 07`，随后为 BER body 长度及以下正文：

| body 偏移 | 字段／编码 |
| --- | --- |
| 0 | 发送计数低 8 位 |
| 1..2 | 内部消息长度 BE16，即数据长度 + 4 |
| 3..4 | 内部 command/type BE16 |
| 5..6 | data length BE16 |
| 7.. | 数据 |

设备回复首字节不保证回显请求计数，不能按计数相等配对请求和回复。发送计数按 8 位回绕。当前校验会话、tag、类型及嵌套长度；软件 generation 不是设备回显字段，不能消除同一会话中重复同类型回复的所有歧义。见 [CI/SAS 编解码](../driver/smit_ci_codec.h) 和 [协议状态机](../driver/smit_protocol.c)。

## 5. 内部命令目录

编号均为 SAS 内部十六进制类型，`REST`、`GSTA` 等字符串只是数据部分，不是完整 USB 报文。

| 命令 | 请求 → 回复 | 数据／回复 | 当前实现与边界 |
| --- | --- | --- | --- |
| Tuner reset | `0001 → 0002` | `REST`；4 字节 BE 结果 | 已实现；保留 CI 会话，不保证恢复全部卡初始化故障 |
| Tune | `0003 → 0004` | 16 字节参数；4 字节 BE 结果 | DVB-C QAM64 已测；结果 0 不等于已锁定 |
| Status | `0005 → 0006` | `GSTA`；16 字节状态 | 已实现、已测；回复频率／符号率须匹配当前调谐 |
| PID blacklist | 名义 `0007 → 0008` | 变长 | 静态构包有疑点；未实现硬件 PID 过滤 |
| Standby | `0009 → 000a` | `STBY`；4 字节 BE 结果 | 有实验代码，默认关闭；睡眠未可靠验收 |
| Resume | `000b → 000c` | `RESU`；4 字节 BE 结果 | 同上 |
| CAM/card info | `0010 → 0011` | `GCIN`；文本 | 已实现；卡状态文本不是节目授权证明 |
| Device type | `0021 → 0022` | `GTYP`；文本 | 已解析，设备类型字段语义待完善 |
| Hardware info | `1007 → 1008` | `GDHW`；文本 | 已解析，接收长度可能使用长形式 |
| Private MMI setting | `1009 → 100a` | 两字节设置／变长回复 | 静态识别；语义未完整确认，未开放 |
| Card notification | 入站 `100c` | 至少一个状态字节 | 有分发代码；真实事件映射仍需验证 |
| Device log | 入站 `1005`；发送 `1002` | 文本／变长 | 已识别；未开放任意写命令 |
| Upgrade data/status | 发送 `1000`；接收 `1001` | 变长 | 仅静态研究，无固件升级实现 |

当前实现将结果 `0` 视为成功；非零结果报错，未定义具体设备错误码语义。升级状态的 `1001` 来自接收队列，不能据此推导“发送 1001 查询”命令。

## 6. DVB-C 调谐参数与 GSTA

Tune 的 16 字节数据使用小端数值，与外层 SAS 命令／长度的大端不同。

| 数据偏移 | 编码 | 当前发送值／解释 |
| --- | --- | --- |
| 0..3 | LE32 | frequency，kHz；Linux Hz 除以 1000 |
| 4..7 | LE32 | symbol rate，kSym/s；Linux symbols/s 除以 1000 |
| 8..11 | LE32 | QAM 数值，当前为 64 |
| 12 | u8 | bandwidth，当前为 8 |
| 13 | u8 | cable 选择标志，当前为 1 |
| 14..15 | u8 × 2 | 当前为 0 |

GSTA 的前 12 字节返回频率、符号率和 QAM 等参数；偏移 13 为 lock，14 为 `level`，15 为固件命名的 `snr`。返回频率和符号率必须与当前调谐匹配后才能使用状态，具体数值由接收网络和信号条件决定。

当前将 `level` 作为绝对强度 dBµV，将 `snr` 作为 0..100 的相对强度，**不是信噪比**。绝对强度按 75 Ω 输入换算为 DVB 毫分贝 dBm：`level * 1000 - 108751`。旧 `FE_READ_SIGNAL_STRENGTH` 使用 `relative * 65535 / 100`；超出 100 则该相对指标不可用。

查询失败、参数不匹配或尚未调谐时不沿用旧指标。当前 `DTV_STAT_CNR` 不可用，不实现可信 RF BER 或 SNR。TEI／CC 统计是 TS 层检查，不能替代射频误码测量。实现见 [frontend](../driver/smit_frontend.c)。

原厂 DTMB 分支中可见将 SR、QAM 及 cable 标志清零的候选参数，但这仅为静态线索，不证明当前驱动支持 DTMB，也不能把旧 `SYS_DVBT` 声明解释为 DTMB 已实现。

## 7. 条件接收与卡状态

| APDU tag | 用途 | 当前边界 |
| --- | --- | --- |
| `9f8030` | CA_INFO enquiry | 请求刷新 CA 信息 |
| `9f8031` | CAID 列表 | 校验偶数字节长度，保存有界列表 |
| `9f8032` | CA_PMT | 当前 PMT 构造后发送到 CA 会话 |
| `9f8033` | CA_PMT_REPLY | 原始 APDU 入队；成功／授权码语义未充分实测 |

CA_PMT 使用 `list_management=3`，保留节目号、版本和节目／ES CA 描述符，在非空 info loop 前加入 `cmd_id=1`。编码与合法性检查见 [CA_PMT](../driver/smit_ca_pmt.h) 和 [用户态 PSI](../tools/server/program_specific_information.h)。默认长度限制也适用于 CA_PMT。

解扰由设备与有授权的智能卡完成，其他节目仍可加扰；当前路径无需主机提取或处理 CW。CAID、slot ready、发送成功或 `9f8033` 的未知值都不能作为授权成功证据。当前仅按单节目选择使用，未验证同时解扰多个节目，也未定义跨固件通用的停止解扰消息。

GCIN 文本用于识别无卡、无效、插反；其他文本保持 unknown。卡文本变化或 `100c` 通知触发 CA 信息失效与刷新，保留有效 CI 会话。文本并非可靠物理在位传感器，unknown 不应解释为有卡或无卡。GCIN 可包含卡标识，应用记录或分享诊断信息前应移除该类字段。

## 8. 复位与研究限制

| 操作 | 作用与限制 |
| --- | --- |
| SAS REST | Tuner 级复位，保留 CI 会话；不保证恢复全部卡初始化故障 |
| 标准 USBDEVFS_RESET | USB 总线级复位，不等于固件冷启动；重建协议会话不保证成功 |
| EP0 vendor reset | `type=40/request=a0/value=0/index=0/length=0`；实验性固件重启路径，可能导致重新枚举与节点中断 |

EP0 请求即使报告 USB 错误也可能已触发重启，不能盲目重复；重枚举后必须重新建链和选台。HTTP 工具只提供显式 `CA_RESET`，没有任意 USB 私有命令入口，驱动具体行为受实验参数及保护条件约束。

当前原始 DVR HTTP 服务不提供自动重选、清流门控或最简 SPTS。协议解析与功能实现不代表持续无错误解码、自动热恢复或全型号兼容性；未完成验证的路径应按实验功能使用。

参考：[nxdong520/iCast](https://github.com/nxdong520/iCast)、[Linux DVB API](https://docs.kernel.org/userspace-api/media/dvb/dvbapi.html)、[usbmon](https://docs.kernel.org/usb/usbmon.html)。返回 [项目说明](../README.md)。
