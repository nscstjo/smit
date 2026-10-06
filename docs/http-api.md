# HTTP API 与客户端工具

## 启动与访问

服务端 `smit` 使用 Linux DVB ioctl；Python 客户端只依赖 Python 3 标准库，可在 Windows、macOS 和 Linux 运行。Windows 和 macOS 未提供原生硬件驱动后端。

```sh
make server
./build/ubuntu-x86_64/smit --bind 127.0.0.1 --port 38212 --adapter /dev/dvb/adapter0
python3 tools/smit.py call /health
python3 tools/smit.py call /api
```

`--mock` 只用于离线模拟，`/health` 会明确报告 `mock:true`；mock 成功不代表硬件通过测试。OpenWrt 配置见 [smit.config](../openwrt/smit/files/smit.config)，服务由 [procd init](../openwrt/smit/files/smit.init) 管理。

默认监听回环地址。远端调用在子命令前指定 `--url http://<服务端地址>:38212`。API 无认证或 TLS，只适用于受控可信网络；拒绝带浏览器 Origin 的请求，不提供宽松 CORS。

## 请求与响应约定

GET 用于健康、路由和流；所有控制操作用 POST，需 `Content-Type: application/json` 和 `Content-Length`，无参数时提交 `{}`。`tools/smit.py call` 不带 JSON 时用 GET，带 JSON 时用 POST。

当前 JSON 解析器只接受平坦对象中的无符号十进制整数和 ASCII 字符串。未知／重复键、布尔值、浮点数、JSON 转义、嵌套对象及尾随内容均会拒绝；不支持 chunked 请求、HTTP 连接复用或流水线。

JSON 响应包含 `ok`，错误还包含 `errno` 和 `error`。`/stream` 成功时返回 `video/mp2t`，不是 JSON。常见状态为 400（输入无效）、409（活动流冲突）、502（DVB 操作失败）；空 CA 队列的 `EAGAIN` 也可能表现为 502。

## 路由

| 方法／路径 | 参数 | 行为／边界 |
| --- | --- | --- |
| GET `/health` | 无 | 进程健康和 mock 状态，不保证硬件就绪 |
| GET `/api` | 无 | 路由列表 |
| POST `/frontend/info` | `{}` | FE_GET_INFO；设备名以 hex 返回 |
| POST `/frontend/tune` | `frequency`、`symbol_rate`，可选 `modulation`，默认 64 | Hz、symbols/s；提交 DVB-C Annex A 调谐，提交成功不等于已锁定 |
| POST `/frontend/status` | `{}` | 锁定及 DVBv5 signal／cnr；缺失统计为 null |
| POST `/ca/caps` | `{}` | CA_GET_CAP |
| POST `/ca/slot` | 可选 `slot`，默认 0 | CA_GET_SLOT_INFO；ready 不表示节目授权 |
| POST `/ca/send` | `hex` | 4..250 字节 APDU；驱动还会校验 tag、结构与发送长度 |
| POST `/ca/receive` | `{}` | 原始 APDU hex、tag 和长度 |
| POST `/ca/reset` | `confirm:1` | 显式 CA_RESET；具体行为受驱动保护条件约束 |
| POST `/demux/section` | `pid`、`table`，可选 `timeout_ms`（100..5000，默认 1000） | 读取 CRC 校验的节表，以 hex 返回 |
| POST `/service/select` | `service`，可选 `timeout_ms`（100..10000，默认 3000） | 从 PAT/PMT 解析节目；需要 CA 时提交 CA_PMT，返回 ES PID／类型 |
| POST `/stream/stats` | `{}` | 活动状态、当前／上一会话发送字节、累计会话／I/O 错误和最后 errno |
| POST `/stream/stop` | `{}` | 取消活动流并等待流句柄关闭 |
| GET `/stream` | 无 | 全复用 PID `0x2000` demux tap，直接复制 `dvr0` 字节 |

API 语法接受 16/32/64/128/256 的 modulation 值，但**当前驱动只支持 64**。更宽的 API 输入范围不代表硬件或驱动支持其他调制。

`/service/select` 的 `clear_verified:false` 表示服务没有验证清流；`ca_submitted:true` 只表示提交成功。其 `streams` 可用于选择客户端监测的目标 PID。signal 的 scale 1 为毫分贝刻度，当前绝对强度以 0.001 dBm 表示；scale 2 为相对刻度。固件相对强度不是 CNR，当前驱动的 cnr 不可用。

## 流连接与诊断

一次只有一个流消费者。第二个连接返回 409；活动流期间调谐、节目选择和复位同样返回 409，先执行 `/stream/stop`。控制查询仍可使用。

服务处理部分写入和 EINTR／EAGAIN；阻塞发送截止时间为 3 秒。DVR 或网络错误关闭流并更新计数。服务不为慢消费者悄悄跳过 TS 包，也不自动重调谐、清除加扰位或修改 PSI/SI。

下例使用 POSIX shell。先将 `FREQUENCY_HZ`、`SYMBOL_RATE` 和 `SERVICE_ID` 设为有效的十进制调谐参数与节目号，再从选节目响应取得 `VIDEO_PID`、`AUDIO_PID`。这些变量不提供默认广播网络值；离线工具示例也使用相同变量。

```sh
python3 tools/smit.py call /frontend/tune "$(printf '{"frequency":%s,"symbol_rate":%s,"modulation":64}' "$FREQUENCY_HZ" "$SYMBOL_RATE")"
python3 tools/smit.py call /frontend/status '{}'
# 确认 locked:true 后，按实际节目号选择。
python3 tools/smit.py call /service/select "$(printf '{"service":%s}' "$SERVICE_ID")"
# VIDEO_PID、AUDIO_PID 须设为选节目响应中的实际 PID。
python3 tools/smit.py stream --seconds 60 --pid "$VIDEO_PID" --pid "$AUDIO_PID" \
  --output capture.ts --report capture.json
```

文件写在客户端，输出／报告路径需不存在；不指定 `--output` 时只监测。`--pid` 不过滤 TS，仍接收全复用流。`clear_verified` 要求每个目标 PID 有清流载荷且观察窗口内没有其加扰包；不代表解码或授权验证。客户端发现流错误、TS 错误或目标未通过检查时返回非零退出码；服务端 I/O 错误计数为 0 也不代表 TS 连续性无错误。

刚连接或选择节目后可能仍有短暂加扰和连续性异常。其他节目可能始终加扰，不能把全复用流任意加扰包当作目标节目失败。服务保留广播 CA 元数据，播放器的加密标记需与目标 TS 检查区分。

## 离线工具

```sh
python3 tools/smit.py analyze-ts capture.ts > tables.json
python3 tools/smit.py analyze-ca capture.ts tables.json > ca-analysis.json
python3 tools/smit.py build-ca-pmt tables.json "$SERVICE_ID" capmt.bin
python3 tools/smit.py monitor-ts output-prefix "$VIDEO_PID" "$AUDIO_PID" < capture.ts
python3 tools/smit.py usbmon-pcap --help
python3 tools/smit.py inspect-sas --help
python3 tools/smit.py analyze-observation observation.log
make check
```

`build-ca-pmt` 仅构造 APDU 文件，不发送给设备；驱动的默认发送长度限制仍适用。`analyze-observation` 用于离线观测日志，不改变当前服务的恢复行为。

实现见 [HTTP 路由](../tools/server/http.c)、[DVB 控制](../tools/server/dvb_control.c)、[客户端入口](../tools/smit.py)。返回 [项目说明](../README.md)。
