# 驱动与服务架构

## 数据与控制路径

```text
tools/smit.py / 播放器
        │ HTTP JSON / 原始 MPEG-TS
        ▼
smit 用户态服务 ── DVB ioctl ── frontend0 / demux0 / ca0
        ▲                              │
        │ dvr0 read                    ▼
Linux DVB 软件 demux          smit ordered 控制工作队列
        ▲                              │ USB 01/82
        │ dvb-usb-v2                   ▼
        └──── USB 84 ─────── 融合电视伴侣多模设备
```

内核处理设备协议及标准 DVB 接入；用户态处理 PAT/PMT、节目选择和 CA_PMT。驱动不会根据订阅音视频 PID 隐式选节目，也不改写广播表。驱动当前仅开放 DVB-C Annex A / QAM64，单模硬件和 DTMB 路径未验证。

## 文件职责

| 文件／目录 | 职责 |
| --- | --- |
| [smit_usb.c](../driver/smit_usb.c) | USB 身份／端点检查、v2 注册、TS feed、设备属性与电源回调 |
| [smit_frontend.c](../driver/smit_frontend.c) | DVB-C 调谐、状态和信号强度映射 |
| [smit_transport.c](../driver/smit_transport.c) | 有截止时间的控制 bulk 收发与外层帧检查 |
| [smit_protocol.c](../driver/smit_protocol.c) | 动态资源会话、SAS 命令、卡信息、故障与复位状态 |
| [smit_worker.c](../driver/smit_worker.c) | ordered 工作队列、请求提交、轮询与启动／停止 |
| [smit_transport_codec.h](../driver/smit_transport_codec.h)、[smit_ci_codec.h](../driver/smit_ci_codec.h) | 有界传输帧、CI/SAS 长度与结构校验 |
| [smit_conditional_access.c](../driver/smit_conditional_access.c) | 高层 CA 字符设备、ioctl／poll 和句柄生命周期 |
| [smit_ca_queue.h](../driver/smit_ca_queue.h)、[smit_ca_pmt.h](../driver/smit_ca_pmt.h) | 有界 CA 队列与 CA_PMT 校验 |
| [tools/server/](../tools/server/) | HTTP、JSON、DVB 控制、PSI、原始 TS 出流和进程运行时 |
| [tools/client/](../tools/client/) | HTTP 调用、录制／监测、TS／CA／USB 离线分析 |
| [openwrt/smit/](../openwrt/smit/) | OpenWrt 包与 procd 服务配置 |
| [tests/](../tests/) | 协议、队列、PSI、服务与客户端离线测试 |

## 控制串行化与生命周期

每设备维护独立状态和 ordered 工作队列，控制端点由该工作队列访问。初始化、前台调谐／查询／CA 请求和后台轮询共享同一串行控制路径。长度错误、短写、超时及会话不匹配向调用者传播，不能沿用陈旧锁定或强度。

解绑时停止新工作并排空／取消工作队列，移除设备节点及 CA 关联，唤醒等待者。CA 对象使用引用计数维持已打开句柄的寿命；解绑后访问返回 `ENODEV`。离线模拟不能复现真实 URB、内核调度与锁竞争，生命周期仍需实机压力和 KASAN／lockdep 验证。

运行时卡事件失效卡／CA 缓存并安排查询，不直接当作冷启动。驱动有有限重试与恢复逻辑，但已热初始化的固件可能忽略冷握手，因此存在恢复代码不代表全部故障可自动恢复。实验性电源、长发送和 EP0 路径默认关闭。

## 标准 DVB 应用契约

| 步骤 | 接口 | 应用需确认 |
| --- | --- | --- |
| 调谐 | `frontend0` / `FE_SET_PROPERTY` | DVB-C Annex A、Hz、symbols/s、QAM64 |
| 等锁 | `FE_READ_STATUS` | `FE_HAS_LOCK` 只表示解调锁定 |
| 读表 | `demux0` / `DMX_SET_FILTER` | 节表 CRC、节目号及当前 PMT |
| 选节目 | `ca0` / `CA_SEND_MSG` | 合法且符合长度限制的 CA_PMT |
| 收流 | demux TS tap + `dvr0` | 需开启 demux filter；可读不等于清流 |
| 播放／录制 | 用户态 | 目标 PID 加扰状态、流质量和实际解码 |

`ca0` 宣告一个高层 `CA_CI` slot，不是 `CA_CI_LINK`，不提供面向 link 层的 read/write；依赖 link 层 EN50221 的应用仍需适配验证。支持 `CA_GET_CAP`、`CA_GET_SLOT_INFO`、`CA_SEND_MSG`、`CA_GET_MSG`、`CA_RESET` 及 poll。发送仅接受已支持的 CA_INFO enquiry 和合法 CA_PMT。

接收队列当前为 16 项。无消息返回 `EAGAIN`，信息失效可报 `ESTALE`，溢出报 `EOVERFLOW`，解绑报 `ENODEV`。slot present/ready 表示嵌入 CI 模块／会话状态，不是卡在位或订阅授权。

## HTTP 服务的边界

服务直接复制 `dvr0` 全复用流，不做清流门控、最简 SPTS、PSI/SI 改写、转码或自动重新选台。播放器自行选节目；客户端 `--pid` 是检查目标，不是过滤输出。一次允许一个流消费者，慢客户端超过发送截止时间会断开。

内核可恢复不代表已断开的客户端会自动恢复。发生故障后由调用者检查状态、停止旧流、重新调谐／选台并重连。

详见 [HTTP API](http-api.md)、[协议研究](device-protocol.md) 和 [项目说明](../README.md)。
