# Qt 上位机备份说明

分支：`backup-qt-zone-control-20260615`

本分支是当前 Qt 上位机代码备份，主要包含 MQTT 数据显示、首页在线状态、传感器页面调整、视频播放地址默认值、工作区/危险区区域控制页面等改动。

## 主要功能

- 通过阿里云 MQTT 接收 RK3588 上报的数据。
- 首页显示 RK3588 和 STM32 在线状态。
- 传感器页面从 MQTT 数据更新温度、烟雾浓度、可燃气体、电源状态、报警状态等信息。
- 区域控制页面用于管理蓝色工作区和红色危险区的自动识别流程。
- 支持“是否重新自动识别”按钮，点击后会通知 RK3588 清空旧区域并重新识别。
- RK3588 离线后，区域控制页面会回到初始状态，等待 RK 重新在线。
- MQTT 发布使用默认自定义 Topic，配合阿里云云产品流转完成 Qt 和 RK 之间通信。

## MQTT 设备和 Topic

Qt 设备：

```text
ProductKey: k29ovUMboAH
DeviceName: 0122-qt
```

Qt 发布命令：

```text
/k29ovUMboAH/0122-qt/user/update
```

Qt 订阅数据：

```text
/k29ovUMboAH/0122-qt/user/get
```

RK3588 设备：

```text
ProductKey: k29ovUMboAH
DeviceName: 0122
```

RK 发布数据：

```text
/k29ovUMboAH/0122/user/update
```

RK 订阅命令：

```text
/k29ovUMboAH/0122/user/get
```

阿里云云产品流转需要配置：

- `/k29ovUMboAH/0122/user/update` 转发到 `/k29ovUMboAH/0122-qt/user/get`
- `/k29ovUMboAH/0122-qt/user/update` 转发到 `/k29ovUMboAH/0122/user/get`

## 区域控制流程

1. RK3588 上电后自动识别画面中的蓝色工作区边框和红色危险区边框。
2. 识别成功后，RK 上报：

```json
{"cmd":"zone_detect_result","ok":1}
```

Qt 显示“区域已划分正常运行”。

3. 如果识别失败，Qt 进入“区域未划分是否继续”的确认流程。
4. 点击“是”后进入人工确认模式，并按倒计时要求定期确认。
5. 点击“否”或确认超时后，Qt 下发关闭/阻止指令，工作区电源保持关闭。
6. 点击“是否重新自动识别”后，Qt 发布：

```json
{"cmd":"zone_redetect_request","enable":0,"mode":"redetect"}
```

RK 收到后重新识别区域。

## 运行方式

在虚拟机中运行：

```bash
cd /home/why/qtpro/QT-feature-reupload
./ngnix_client
```

如果需要重新编译：

```bash
cd /home/why/qtpro/QT-feature-reupload
make -j4
```

## 关键文件

- `PageZone.cpp/.h`：工作区/危险区区域控制页面。
- `AliyunMqttClient.cpp/.h`：阿里云 MQTT 连接、订阅、发布封装。
- `mainwindow.cpp/.h`：页面集成和在线状态显示。
- `page_home.cpp/.h`：首页状态显示。
- `page_sensor.cpp/.h`：传感器、报警状态、电源状态显示。
- `ngnix_client.pro`：Qt 工程文件。

## 注意事项

- Qt 和 RK3588 不能使用同一个 MQTT `clientId`，否则阿里云会让其中一个设备掉线。
- Qt 只订阅自己的 `/k29ovUMboAH/0122-qt/user/get`，RK 的消息依靠阿里云云产品流转转发过来。
- 如果区域控制页显示 RK 离线，需要先确认 RK 是否正在运行、MQTT 是否连接成功、云产品流转是否启动。
- 如果点击“是否重新自动识别”没有反应，优先看 RK 端是否收到 `zone_redetect_request`。

