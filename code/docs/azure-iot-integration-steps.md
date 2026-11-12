# Microsoft Azure IoT 对接设置步骤文档

> 本文档整理设备对接 Azure IoT Hub / DPS 所需的全部设置项与操作步骤。
> 适配场景：ESP32 等嵌入式设备通过 MQTT 直连 Azure IoT Hub。
> 参考官方文档：
> - IoT Hub MQTT 直连：https://learn.microsoft.com/en-us/azure/iot/iot-mqtt-connect-to-iot-hub
> - SAS 令牌：https://learn.microsoft.com/en-us/azure/iot-hub/iot-hub-dev-guide-sas
> - DPS 对称密钥注册：https://learn.microsoft.com/en-us/azure/iot-dps/quick-create-simulated-device-symm-key

---

## 一、总体连接架构

设备连接 Azure IoT 有两种路径：

| 路径 | 说明 | 适用 |
|------|------|------|
| A. 直连 IoT Hub | 设备携带 HostName + DeviceId + SAS Token 直接连 IoT Hub | 少量设备、固定设备、原型验证 |
| B. 经 DPS 自动注册 | 设备用 ID Scope + Registration ID + 对称密钥向 DPS 注册，由 DPS 分配 IoT Hub | 量产、多设备、需分配策略 |

本文档两条路径都覆盖，先做路径 A 打通，再按需升级到路径 B。

---

## 二、连接参数总表（对接必填项）

### 2.1 网络与协议设置（直连 IoT Hub）

| 参数 | 取值 | 备注 |
|------|------|------|
| 协议 | MQTT v3.1.1 | IoT Hub 不支持 MQTT v5 |
| 端口（MQTT） | **8883** | 必须 TLS 1.2 |
| 端口（MQTT over WebSocket） | 443 | 防火墙封 8883 时使用，路径 `/mqtt` |
| 端口 1883 | ❌ 不支持 | 明文，禁止 |
| TLS 版本 | **TLS 1.2** | 跳开会连接失败 |
| 根证书 | DigiCert Global Root G2 | 用于校验服务端证书 |
| Keep-Alive | ≤ 1177 秒（建议 240s） | 服务端上限 1767s，客户端值×1.5 不得超过 |

### 2.2 MQTT CONNECT 包字段

| 字段 | 取值格式 | 示例 |
|------|----------|------|
| **ClientId** | 设备 DeviceId | `MyDevice01` |
| **Username** | `{iothub-hostname}/{device-id}/?api-version=2021-04-12` | `contoso.azure-devices.net/MyDevice01/?api-version=2021-04-12` |
| **Password** | SAS Token | `SharedAccessSignature sr=...&sig=...&se=...` |
| CleanSession | 0 | SDK 默认 |
| QoS | 1 | 推荐；QoS 0 不保证送达 |

### 2.3 遥测 Topic 设置

| 用途 | Topic |
|------|-------|
| 上行遥测（D2C） | `devices/{device-id}/messages/events/` |
| 上行遥测带属性 | `devices/{device-id}/messages/events/{property-bag}` |
| Will 消息 Topic | `devices/{device-id}/messages/events/` |
| 下行消息（C2D） | `devices/{device-id}/messages/devicebound/#` |

### 2.4 SAS Token 结构

格式：
```
SharedAccessSignature sr={URL-encoded-resourceURI}&sig={signature-string}&se={expiry}&skn={policyName}
```

| 组成 | 含义 |
|------|------|
| `sr` | 资源 URI（小写 URL 编码）。设备级：`{hub}.azure-devices.net/devices/{deviceId}` |
| `sig` | HMAC-SHA256(`sr` + "\n" + `se`, base64解码后的密钥)，再做 base64 + URL 编码 |
| `se` | 过期时间（Unix 秒，UTC） |
| `skn` | 共享访问策略名；用设备对称密钥时**省略** |

生成命令（Azure CLI）：
```shell
az iot hub generate-sas-token \
  --hub-name <hub-name> \
  --device-id <device-id> \
  --duration 3600
```

---

## 三、路径 A：直连 IoT Hub 操作步骤

### 步骤 1：创建 IoT Hub
1. 登录 [Azure Portal](https://portal.azure.com/)。
2. 搜索 **IoT Hub** → **Create**。
3. 选择订阅、资源组、区域，填写 **Hub name**（即 hostname 前缀）。
4. 层级选择 **Standard**（若需设备孪生/方法/C2D 必须 Standard）。
5. 创建完成，记录：
   - **Hostname**：`<hub-name>.azure-devices.net`

### 步骤 2：注册设备身份
1. 进入 IoT Hub → **Devices** → **Add Device**。
2. 填写 **Device ID**（如 `MyDevice01`）。
3. 认证方式选择 **Symmetric Key**（测试可用），或 X.509 证书（推荐生产）。
4. 保存后记录：
   - **Device ID**
   - **Primary Key / Secondary Key**（对称密钥，base64）

### 步骤 3：生成 SAS Token
1. 用步骤 2 的 Device ID + Primary Key 生成 SAS Token（CLI 命令见 2.4）。
2. 或用 VS Code 的 Azure IoT Hub 扩展右键设备 → Generate SAS Token。
3. 记录 **SAS Token**（注意过期时间，到期需重新生成或改用设备端自动续签）。

### 步骤 4：准备 TLS 根证书
1. 下载 **DigiCert Global Root G2** 根证书（`.cer`）。
2. 转换为可嵌入格式（BEGIN/END CERTIFICATE，去掉引号和 `\r\n`）。
3. 烧录/内置到设备固件中，用于 TLS 校验。

### 步骤 5：配置设备连接参数
在设备代码中填入：
```
mqtt_host      = "<hub-name>.azure-devices.net"
mqtt_port      = 8883
client_id      = "<device-id>"
username       = "<hub-name>.azure-devices.net/<device-id>/?api-version=2021-04-12"
password       = "<sas-token>"
tls_ca_cert    = <DigiCert Global Root G2>
keepalive      = 240
clean_session  = 0
qos            = 1
```

### 步骤 6：连接并验证
1. 设备连接 8883 + TLS 1.2。
2. 向 `devices/<device-id>/messages/events/` 发布一条测试消息。
3. 在 Portal → IoT Hub → **Overview → Usage / 或用"Device Explorer / az iot hub monitor-events"** 查看是否收到遥测。
```shell
az iot hub monitor-events --hub-name <hub-name> --device-id <device-id>
```

---

## 四、路径 B：DPS 自动注册操作步骤（量产用）

### 前置：创建 DPS 并链接 IoT Hub
1. Portal 创建 **Device Provisioning Service**（DPS）。
2. DPS → **Linked IoT hubs** → 添加步骤 A 中创建的 IoT Hub。
3. 记录 DPS **Overview** 中的：
   - **ID Scope**

### 步骤 1：创建单个/组注册
1. DPS → **Manage enrollments** → **Individual enrollments** → **Add**。
2. **Attestation** 选择 **Symmetric key**。
3. 勾选 **Generate symmetric keys automatically**（或自备密钥）。
4. 填写 **Registration ID**（设备唯一）。
5. 勾选 **Enable this enrollment**。
6. **IoT hubs** 页：选择目标 hub 与 **Allocation policy**。
7. **Device settings** 页：可填 **Device ID**（不填则用 Registration ID）。
8. 创建后记录：
   - **Registration ID**
   - **Primary Key**

### 步骤 2：设备端注册参数
```
dps_id_scope         = "<ID Scope>"
dps_registration_id  = "<Registration ID>"
dps_symmetric_key    = "<Primary Key>"
attestation_type     = SYMMETRIC_KEY
dps_global_endpoint  = global.azure-devices-provisioning.net
dps_port             = 8883
```

### 步骤 3：执行注册
1. 设备向 DPS `global.azure-devices-provisioning.net:8883`（TLS 1.2）发起注册。
2. 使用 ID Scope + Registration ID + 对称密钥认证。
3. DPS 返回分配结果：
   ```
   assigned hub = <hub-name>.azure-devices.net
   device id    = <device-id>
   ```
4. 设备再用该 hub 的 HostName + DeviceId + SAS Token 走路径 A 的连接流程。

---

## 五、防火墙与网络设置

| 项目 | 设置 |
|------|------|
| 出站端口 | 允许 TCP **8883**（MQTT/TLS），或 443（WebSocket） |
| DNS | 能解析 `*.azure-devices.net`、`global.azure-devices-provisioning.net` |
| NTP/时间 | 设备时间需准确，否则 SAS `se` 校验失败 |
| 代理 | 若经代理，需支持 TLS 1.2 直通 |

---

## 六、安全建议

1. **优先使用 X.509 证书认证**，SAS 对称密钥仅用于测试评估。
2. SAS Token 应设置合理过期时间（如 1 小时），设备端实现自动续签。
3. 私钥/对称密钥不下发明文到不信任环境，固件中加密存储。
4. 生产用 DPS + 组注册 + 分配策略，避免逐台手工配置。

---

## 七、对接检查清单（Checklist）

- [ ] IoT Hub 已创建并记录 **Hostname**
- [ ] 设备已在 Identity Registry 注册，记录 **Device ID** 与 **Key**
- [ ] 已生成 **SAS Token**（未过期）或已准备 X.509
- [ ] 已内置 **DigiCert Global Root G2** 根证书
- [ ] 端口 **8883**（或 443）出站放通
- [ ] 设备时间准确（NTP 已同步）
- [ ] CONNECT 包 ClientId/Username/Password 格式正确
- [ ] 遥测 Topic 为 `devices/{device-id}/messages/events/`
- [ ] TLS 版本 = **1.2**
- [ ] 能通过 `az iot hub monitor-events` 看到上行消息
- [ ]（量产）DPS **ID Scope / Registration ID / Primary Key** 已配置并可注册成功
```
