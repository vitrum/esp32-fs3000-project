# ENV 配置

本机账号配置放在项目根目录 `.env`，可提交的空白模板为 `.env.example`。
当前本地文件未填入真实 Wi-Fi、IoT 或 OSS 凭据。

## 字段

| 用途 | 字段 | 当前行为 |
| --- | --- | --- |
| Wi-Fi | `WIFI_SSID`、`WIFI_PASSWORD` | 无已保存网络时的可选默认值 |
| IoT 服务 | `IOT_ENDPOINT`、`IOT_DEVICE_ID`、`IOT_USERNAME`、`IOT_PASSWORD`、`IOT_TOKEN` | 配置入口预留，未连接云服务 |
| OSS 目标 | `OSS_ENDPOINT`、`OSS_BUCKET`、`OSS_REGION`、`OSS_FIT_PREFIX` | 上传目标预留，建议限定到 `fit/` 前缀 |
| 设备上传 | `OSS_SIGN_URL_ENDPOINT`、`OSS_FIT_UPLOAD_URL` | 后端签名服务或短期预签名上传 URL，未实现上传客户端 |
| 主机/后端 OSS 凭据 | `OSS_ACCESS_KEY_ID`、`OSS_ACCESS_KEY_SECRET`、`OSS_SECURITY_TOKEN` | 仅留在主机，明确不生成进固件 |
| 固件下载 | `FIRMWARE_MANIFEST_URL`、`FIRMWARE_DOWNLOAD_URL`、`FIRMWARE_EXPECTED_SHA256` | 下载与校验配置预留，未启用 OTA |

OSS 和固件 URL 必须为 HTTPS，不允许把账号密码写进 URL 的用户信息段。
预签名 URL 可以包含查询参数；它也是敏感信息，且通常有有效期。
`FIRMWARE_EXPECTED_SHA256` 非空时必须是 64 位十六进制摘要。
SHA256 只能验证内容完整性，不能代替可信签名和版本回滚保护。

## 加载方式

1. 编译时读取 `.env`，同名进程环境变量优先，可用于 CI 注入。
2. 构建钩子生成 `.pio/build/esp32s3/generated/aero_env_config.h`，仅包含明确允许的设备字段。
3. 固件通过 `src/env_settings.h` 的 `EnvSettings` 接口读取配置。
4. Wi-Fi 优先使用 `aero-links` 中保存的网络，其次兼容旧 `aero-wifi`，最后才使用 ENV 默认值。
5. 成功的配网连接仍按现有机制保存；ENV 不覆盖 ESPTouch 配网结果。

修改 `.env` 后需重新编译，再按授权的应用区烧录流程部署；这是编译配置，不是设备上的实时设置文件。
缺少 `.env` 时字段为空，不强制联网。构建钩子不打印配置值。
支持 `KEY=value` 和带引号的字符串、整行 `#` 注释；不执行 shell，不展开 `$VAR`。
密码中需要保留首尾空格或引号时使用引号。多行 PEM 等内容应使用独立文件，不能放在单行 ENV 字段中。

```sh
~/.platformio/penv/bin/pio run -e esp32s3
```

## 安全边界

- `.env` 及其本地变体被 Git 和 Docker 构建上下文忽略，模板例外；本机和生成头文件使用仅所有者读写权限。
- 不要提交密码、AccessKey、STS Token、预签名 URL 或带凭据的构建产物；忽略规则不能移除已经提交的凭据。
- 忽略规则、文件权限和字符编码都不是加密。实际使用的设备凭据仍可能从固件或设备中提取。
- 不向设备下发长期云管理密钥。FIT 上传优先由可信后端签发限制对象路径、权限和有效期的凭证。
- 当前分区表只有一个应用分区，尚不具备常规双分区 OTA 的部署条件。固件下载不等于自动更新；后续必须单独实现可信校验、安全安装和相应分区迁移。

## 验证

`test/host/test_env_config.py` 检查默认值、CI 覆盖、字符与长度、HTTPS/SHA256、云密钥隔离、删除配置后的清空以及 C++11 头文件编译。
ENV 测试已加入 `sh test/run.sh host` 和 `all`，单独报告可运行：

```sh
docker run --rm -v "$PWD:/project" aeroprobe-idf-test:4.4.6 /opt/pytest-venv/bin/python -m pytest -q test/host/test_env_config.py --junitxml=test/results/env-config.xml
```