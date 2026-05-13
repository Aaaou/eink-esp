# 墨水屏 BLE 命令表

本文档描述当前微信小程序 `D:\WECHAT_DEMO\BLE_EINK` 所使用的 BLE 自定义服务、特征值以及业务命令，便于后续设备端与小程序联调。

## 1. 服务与特征值

### 主服务 UUID

- `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`

### RX 写入特征值

- UUID: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- 方向: 小程序 -> 设备
- 用途:
  - 写入命令包
  - 写入图像分片

### TX 通知特征值

- UUID: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
- 方向: 设备 -> 小程序
- 用途:
  - ACK
  - 进度通知
  - 结果通知
  - 错误通知

### STATE 读取特征值

- UUID: `6E400004-B5A3-F393-E0A9-E50E24DCCA9E`
- 方向: 小程序读取
- 用途:
  - 读取设备信息摘要
  - 读取模式/忙碌状态

## 2. 协议头

所有命令使用固定 10 字节头：

- `0`: `version`
- `1`: `msgType`
- `2`: `opcode`
- `3`: `flags`
- `4-5`: `sessionId` 小端
- `6-7`: `seq` 小端
- `8-9`: `payloadLen` 小端

当前版本：

- `version = 0x01`

## 3. 消息类型

- `0x01`：命令请求
- `0x02`：命令响应
- `0x03`：数据分片
- `0x04`：ACK
- `0x05`：事件/进度/结果
- `0x06`：错误

## 4. 业务命令

### 控制类

- `0x01`：`GET_DEVICE_INFO`
- `0x02`：`GET_CAPABILITIES`
- `0x03`：`ABORT_SESSION`
- `0x04`：`PING`

### 墨水屏业务类

- `0x10`：`SHOW_CALENDAR`
- `0x11`：`SHOW_MEMO`
- `0x12`：`BEGIN_FRAME_UPLOAD`
- `0x13`：`FRAME_CHUNK`
- `0x14`：`END_FRAME_UPLOAD`

### 事件类

- `0x20`：`PROGRESS`
- `0x21`：`RESULT`
- `0x22`：`ERROR_CODE`

## 5. TLV 规则

命令体使用 TLV：

- `type`：1 字节
- `len`：2 字节，小端
- `value`：变长

## 6. 命令说明

### 6.1 获取设备信息

- `opcode = 0x01`
- `payloadLen = 0`

设备应返回：

- 固件版本
- 硬件型号
- 面板宽度
- 面板高度
- 颜色模式
- 最大写入负载
- 忙碌状态

### 6.2 日历命令

- `opcode = 0x10`

TLV：

- `0x01`：Unix 时间戳 `uint32 LE`
- `0x02`：时区偏移分钟数 `int16 LE`，可选

语义：

- 设备端根据时间戳本地渲染日历

### 6.3 备忘录命令

- `opcode = 0x11`

TLV：

- `0x01`：条目数 `uint8`
- `0x10`：第 1 条 memo
- `0x11`：第 2 条 memo
- `0x12`：第 3 条 memo

单条 memo value：

- `byte0`：是否勾选
  - `0x00` 未勾选
  - `0x01` 已勾选
- 后续字节：UTF-8 文本

约束：

- 最多 3 条
- 文本最大 31 字节

### 6.4 图像上传开始

- `opcode = 0x12`

TLV：

- `0x01`：宽 `uint16 LE`
- `0x02`：高 `uint16 LE`
- `0x03`：刷新模式 `uint8`
  - `0` 三色全刷
  - `1` 黑白快刷
- `0x04`：平面格式 `uint8`
- `0x05`：总字节数 `uint32 LE`
- `0x06`：期望分片大小 `uint16 LE`
- `0x07`：算法提示 `uint8`，可选

### 6.5 图像上传分片

- `opcode = 0x13`

TLV：

- `0x01`：分片偏移 `uint32 LE`
- `0x02`：分片内容

### 6.6 图像上传结束

- `opcode = 0x14`

TLV：

- `0x01`：CRC32 `uint32 LE`
- `0x02`：总字节数 `uint32 LE`

## 7. 当前图像帧格式

目标面板：

- 宽 `104`
- 高 `212`

单平面字节数：

- `104 * 212 / 8 = 2756`

当前上传帧总格式：

- `byte0`：刷新模式
- `byte1 ~ byte2756`：黑色平面
- `byte2757 ~ byte5512`：红色平面

总长度：

- `5513` 字节

## 8. 当前小程序已覆盖功能

小程序代码已实现并可调用：

- 获取设备信息
- 发送日历命令
- 发送备忘录命令
- 图像处理
- 图像帧上传

对应文件：

- `D:\WECHAT_DEMO\BLE_EINK\miniprogram\utils\bleConstants.js`
- `D:\WECHAT_DEMO\BLE_EINK\miniprogram\utils\bleProtocol.js`
- `D:\WECHAT_DEMO\BLE_EINK\miniprogram\utils\einkBleClient.js`
- `D:\WECHAT_DEMO\BLE_EINK\miniprogram\utils\imageProcessor.js`

## 9. 当前设备端实现目标

本阶段设备端最少应完成：

- 广播自定义 BLE 服务
- 注册上述 3 个特征值
- 能接收并解析：
  - `GET_DEVICE_INFO`
  - `SHOW_CALENDAR`
  - `SHOW_MEMO`
  - `BEGIN_FRAME_UPLOAD`
  - `FRAME_CHUNK`
  - `END_FRAME_UPLOAD`
- 能通过日志确认命令已进入设备端

后续再补：

- ACK/RESULT/PROGRESS 真正通知
- CRC 校验
- 真实图像显示调用
