# waterTest 项目说明

`waterTest` 是一个基于 ESP32 的供暖/水路监测固件，用于采集热电偶温度、检测漏水、驱动 OLED 显示、联动 Blinker App 报警，并将网络配置与阈值保存到 NVS 中。

核心实现文件是 [watertest.ino](watertest.ino)，本文档按“硬件连接 -> 功能 -> 配置 -> 关键代码”顺序说明，便于直接交付和后续维护。

## 1. 项目功能

本项目完成了下面几类工作：

- 读取 MAX31855 热电偶模块的热端温度和冷端温度
- 对热端温度做线性拟合后显示和上报
- 读取漏水检测输入，支持低电平触发
- 使用 u8g2 驱动 128x64 OLED 显示启动、数据和告警界面
- 通过 Blinker App 显示温度、阈值和漏水状态
- 在温度过高或漏水时发送通知并驱动蜂鸣器报警
- 支持通过串口修改 WiFi SSID、WiFi 密码和 Blinker auth
- 使用 ESP32 Preferences 保存配置，掉电不丢失

## 2. 硬件组成

### 2.1 主控与外设

- ESP32
- MAX31855 热电偶模块
- 漏水检测模块
- OLED 128x64 I2C 显示屏
- 蜂鸣器

### 2.2 默认引脚

| 功能 | 引脚 | 说明 |
| --- | --- | --- |
| MAX31855 SCK | GPIO 13 | SPI 时钟 |
| MAX31855 CS | GPIO 12 | 片选 |
| MAX31855 DO | GPIO 14 | SPI 数据输出 |
| 漏水输入 | GPIO 25 | 默认低电平触发 |
| 蜂鸣器 | GPIO 2 | 使用 LEDC PWM 输出 |
| OLED SDA | GPIO 33 | I2C 数据线 |
| OLED SCL | GPIO 32 | I2C 时钟线 |

### 2.3 关键参数

| 参数 | 默认值 | 作用 |
| --- | --- | --- |
| 采样周期 | 500 ms | 读取温度和漏水状态 |
| 上报周期 | 1000 ms | 向 Blinker 上传数据 |
| 温度阈值 | 60.0°C | 超温报警门限 |
| 蜂鸣器频率 | 2200 Hz | 报警音调 |
| 蜂鸣器节奏 | 200 ms / 800 ms | 慢响报警 |

## 3. Blinker App 配置

Blinker 配置文件中对应的控件 key 如下：

- `num-tem`：热端温度
- `num-hhp`：冷端温度
- `ran-bdm`：温度阈值
- `btn-bs`：消除警报

### 3.1 App 页面建议

建议在 Blinker 中放置 3 个数值控件和 1 个按钮控件：

- 热端温度：`num-tem`
- 冷端温度：`num-hhp`
- 阈值调节：`ran-bdm`
- 消音按钮：`btn-bs`

### 3.2 数据含义

- `num-tem` 显示的是经过拟合后的热端温度
- `num-hhp` 显示 MAX31855 的冷端温度
- `ran-bdm` 会写回到设备并保存到 NVS
- `btn-bs` 只负责关闭当前蜂鸣器报警，不会删除阈值或网络配置

## 4. 功能说明

### 4.1 温度采集与拟合

程序直接读取 MAX31855 的热端温度，再通过线性拟合换算为显示值。代码里使用了两点标定：

- 显示 60°C 时，实际约 23°C
- 显示 230°C 时，实际约 90°C

换算公式为：

$$
T_{real} = 0.3941176 \times T_{display} - 0.6470588
$$

其中，`T_display` 是 MAX31855 读到的热端温度，`T_real` 是用于显示、上报和超温判断的温度。

### 4.2 漏水检测

漏水输入默认按低电平触发。检测到漏水后，固件会：

- 切换 OLED 为报警界面
- 向 Blinker App 推送漏水通知
- 启动蜂鸣器慢响

### 4.3 超温报警

当拟合后的热端温度大于当前阈值时，固件判定为超温：

- OLED 显示报警界面
- 向 Blinker App 推送“温度过高，请检查水管”
- 蜂鸣器慢响

### 4.4 蜂鸣器逻辑

蜂鸣器使用 LEDC 输出方波，默认以慢速间歇鸣叫的方式提示报警。

- 漏水报警和超温报警共用同一套蜂鸣器逻辑
- 按下 `btn-bs` 后会关闭当前报警
- 下一次新的漏水或超温事件出现时，会自动重新响起

### 4.5 OLED 显示逻辑

OLED 使用 u8g2 驱动，包含 3 种状态：

- 启动画面
- 正常数据画面
- 告警画面

正常画面会同时显示：

- 热端温度
- 冷端温度
- 当前阈值
- 漏水状态
- 当前是否超温

## 5. 串口配置

串口参数：`115200`，建议行尾选择 `CRLF`。

设备上电后会自动打印帮助信息和当前配置，便于现场调试。

### 5.1 可用命令

查看帮助：

```text
help
```

查看当前配置：

```text
show
```

同时设置 WiFi 和 Blinker auth：

```text
cfg <ssid> <password> <blinkerauth>
```

只设置 WiFi：

```text
set wifi <ssid> <password>
```

只设置 Blinker auth：

```text
set auth <blinkerauth>
```

一次性设置全部：

```text
set all <ssid> <password> <blinkerauth>
```

### 5.2 带空格参数

如果参数里有空格，可以用双引号：

```text
set wifi "My Home WiFi" "12345678"
```

### 5.3 保存行为

当执行 `cfg`、`set wifi`、`set auth` 或 `set all` 成功后，程序会：

1. 将配置写入 NVS
2. 立即读回并校验
3. 打印当前配置
4. 自动重启
5. 使用新配置重新连接 WiFi 和 Blinker

这意味着配置不是只改内存，而是真正持久化保存了。

## 6. 运行流程

固件启动后，整体流程如下：

1. 初始化串口
2. 打开 Preferences，并从 NVS 读取配置
3. 若配置无效，则加载默认值并重新保存
4. 初始化 Blinker、漏水输入、蜂鸣器和 OLED
5. 显示启动页
6. 首次读取 MAX31855，确认传感器是否在线
7. 进入 `loop()` 后持续处理串口命令和 Blinker 数据
8. 定时采集温度和漏水状态
9. 条件满足时发送通知、刷新界面、更新蜂鸣器和 App 数据

## 7. 关键代码说明

下面这些函数是项目的核心逻辑，理解它们基本就能掌握整个固件。

### 7.1 配置结构与校验

```cpp
struct DeviceConfig
{
	uint32_t magic;
	uint32_t version;
	float tempThresholdC;
	char blinkerAuth[BLINKER_AUTH_MAX_LEN + 1];
	char wifiSsid[WIFI_SSID_MAX_LEN + 1];
	char wifiPswd[WIFI_PSWD_MAX_LEN + 1];
	uint32_t checksum;
};
```

这部分定义了设备持久化配置，包括：

- 配置标识 `magic`
- 版本号 `version`
- 温度阈值
- WiFi 和 Blinker 凭据
- 校验值 `checksum`

配套的 `computeConfigChecksum()` 会对整份配置做哈希，避免 NVS 中数据损坏后被误认为有效。

### 7.2 读取和写入 NVS

```cpp
bool saveConfig()
{
	DeviceConfig config = {};
	config.magic = EEPROM_MAGIC;
	config.version = CONFIG_VERSION;
	config.tempThresholdC = tempThresholdC;
	copyCString(config.blinkerAuth, sizeof(config.blinkerAuth), blinkerAuth);
	copyCString(config.wifiSsid, sizeof(config.wifiSsid), wifiSsid);
	copyCString(config.wifiPswd, sizeof(config.wifiPswd), wifiPswd);
	config.checksum = computeConfigChecksum(config);
	...
}
```

`saveConfig()` 负责把当前内存里的阈值、WiFi 和 auth 写入 NVS，并立即回读校验。这个设计比“只写不验”更稳，适合现场部署。

```cpp
void loadConfig()
{
	DeviceConfig config = {};
	size_t readBytes = devicePrefs.getBytes("cfg", &config, sizeof(config));
	...
}
```

`loadConfig()` 先尝试从 NVS 读取，如果校验失败，就回退到默认配置并再次写入。

### 7.3 串口命令解析

```cpp
void handleSerialCommand(String line)
{
	line.trim();
	...
	if (command == "cfg")
	{
		String ssid = nextSerialToken(line, index);
		String password = nextSerialToken(line, index);
		String auth = nextSerialToken(line, index);
		...
	}
}
```

串口命令支持：

- `help`
- `show`
- `cfg`
- `set wifi`
- `set auth`
- `set all`

其中 `nextSerialToken()` 支持带引号参数，所以串口里输入带空格的 WiFi 名称也能正常解析。

### 7.4 App 数据处理

```cpp
void handleAppData(const String &data)
{
	int silenceIndex = data.indexOf("btn-bs");
	if (silenceIndex >= 0)
	{
		buzzerSilenced = true;
		setBuzzerOutput(false);
	}
	...
}
```

这个回调会接收 Blinker 下发的数据，并做两件事：

- 识别 `btn-bs`，执行消音
- 解析 `ran-bdm`，把阈值写回设备并保存到 NVS

也就是说，App 上调阈值后会立即生效，并且重启后仍然保留。

### 7.5 温度上传和状态同步

```cpp
void uploadBlinkerData()
{
	if (lastTcOk)
	{
		NumberTem.print(getTcDisplayTemp(lastTcTempC));
		NumberHhp.print(lastCjTempC);
	}
	else
	{
		NumberTem.print(0);
		NumberHhp.print(0);
	}

	NumberThreshold.print(tempThresholdC);
	Blinker.print("water_leak", lastLeak ? 1 : 0);
}
```

这部分负责把设备当前状态同步到 Blinker：

- 热端温度
- 冷端温度
- 当前阈值
- 漏水状态

### 7.6 告警触发

```cpp
void notifyIfNeeded(bool leak, bool tempHigh)
{
	if (tempHigh && !lastTempHigh)
	{
		buzzerSilenced = false;
		Blinker.notify("温度过高，请检查水管");
	}

	if (leak && !lastLeakAlert)
	{
		buzzerSilenced = false;
		Blinker.notify("检测到漏水，请检查水管");
	}
}
```

这里做的是“边沿触发”告警：只有从正常变成异常时才发通知，避免每次循环都刷屏。

### 7.7 主循环

```cpp
void loop()
{
	pollSerialCommands();
	Blinker.run();
	...
	bool leak = isWaterLeak();
	double tcTempC = thermocouple.readCelsius();
	double cjTempC = thermocouple.readInternal();
	...
}
```

`loop()` 是整个项目的调度中心，核心动作是：

- 持续读取串口命令
- 维持 Blinker 通讯
- 按周期采样温度和漏水状态
- 更新 OLED
- 更新蜂鸣器
- 定时向 App 上报数据

## 8. 代码中的主要配置项

这些常量决定了项目的默认行为，修改时通常只需要看这里：

- `PIN_MAX31855_SCK` / `PIN_MAX31855_CS` / `PIN_MAX31855_DO`：MAX31855 引脚
- `PIN_WATER_LEAK`：漏水检测输入
- `PIN_BUZZER`：蜂鸣器输出
- `PIN_OLED_SDA` / `PIN_OLED_SCL`：OLED I2C 引脚
- `DEFAULT_TEMP_THRESHOLD_C`：默认温度阈值
- `HOT_TEMP_FIT_SLOPE` / `HOT_TEMP_FIT_INTERCEPT`：温度拟合参数
- `SAMPLE_INTERVAL_MS`：采样周期
- `BLINKER_UPLOAD_INTERVAL_MS`：上报周期
- `DEFAULT_WIFI_SSID` / `DEFAULT_WIFI_PSWD` / `DEFAULT_BLINKER_AUTH`：首次启动默认配置

## 9. 现场使用建议

1. 先按默认引脚把硬件接好
2. 上电后先看串口输出，确认 MAX31855、NVS 和 Blinker 初始化是否正常
3. 用串口命令设置自己的 WiFi 和 Blinker auth
4. 在 Blinker 中建立对应控件，确保 key 与固件一致
5. 检查 OLED、串口和 App 的数据是否同步
6. 最后再根据实际温度校准情况微调阈值或拟合参数

## 10. 常见问题

### 10.1 App 连不上

优先检查：

- WiFi SSID 是否正确
- WiFi 密码是否正确
- Blinker auth 是否正确
- 手机和 ESP32 是否处在可联网环境中

### 10.2 App 没有显示数值

优先检查：

- App 控件是否确实是数值控件
- key 是否分别为 `num-tem`、`num-hhp`、`ran-bdm`
- 设备是否已经成功连上 Blinker

### 10.3 蜂鸣器不响

优先检查：

- 是否接在 GPIO 2
- 当前是否真的达到漏水或超温条件
- 是否被 `btn-bs` 消音

### 10.4 漏水或超温提示不出现

优先检查：

- `WATER_ACTIVE_LEVEL` 是否和传感器输出一致
- 温度拟合是否适合当前硬件
- 阈值是否设置得过高

## 11. 备注

项目默认配置只适合首次启动或 NVS 未初始化时使用。实际部署时，建议在串口里重新设置为自己的 WiFi、Blinker auth 和阈值，然后再做现场安装。
