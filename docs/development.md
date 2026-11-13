# 开发与设备说明

## 渲染与刷新

运行时由设备上的 C++17 程序完成数据获取、缓存、FreeType / HarfBuzz 排版、8 位灰度绘图和 FBInk 提交。Python 只用于开发阶段的素材准备与对比检查，不部署到 Kindle。

逻辑画布为 **1440×1080**。KPW3 物理屏幕为 **1072×1448**：裁去逻辑画布上下各 4 像素空白，左右各补 4 像素，再旋转输出；文字和曲线不缩放。局部矩形按相同规则旋转，覆盖整屏且不重叠。

左侧 `clock`、`weather`、`sunset`、`usage` 和右侧每行 `market:N` 分别比较像素。即使接口返回变化，只要最终像素不变，就不提交 FBInk。网络线程并行请求，主线程统一提交屏幕。云图合并天气与日落区域，新闻替换右侧行情区域。

```yaml
refresh:
  poll_seconds: 1
  full_refresh_seconds: 900  # 0 关闭周期全刷；启动和布局变化仍全刷
  market_seconds: 60
  weather_seconds: 600
  workers: 4                # 1–8
  regions:
    market:0: 30            # 单独覆盖第 1 行行情的间隔，单位秒
    usage: 60
    sunset: 600
display:
  rotation: 90              # 90 / 270
  waveform: GC16            # GC16 / GL16 / AUTO；全刷固定 GC16
  fbink: fbink              # 可指定设备上的绝对路径
```

时钟按本地分钟更新，全刷计时使用单调时钟。`--freeze-time` 仅供演示和对比检查。`schedule.timezone` 指定时区；旧配置中的服务器字段和 `schedule.cron` 不参与原生循环。

行情备注使用 `symbol[English note]`；不支持括号候补源语法。内置 XSHG / XNYS 交易日历有效至 **2026-12-31**；更新时先升级到包含目标年份的 `exchange-calendars` 版本，再调整 `tools/prepare_assets.py` 的日期范围并重新生成。

## 数据与缓存

- 自定义行情从本地 `market.custom_symbols` 配置读取地址和产品名，以 `date=YYYY-MM-DD` 和 `product` 请求。数据行使用 `time`（毫秒时间戳）、`ret` 和可选 `exposure`，按交易分钟采样。
- 天气默认使用 Open-Meteo；日出日落预测使用配置的 Sunset 服务，继续遵守 `refresh_seconds` 和 `cache_max_age_seconds`。
- 云图使用 MET Norway Locationforecast complete；降雨为各时间起未来 6 小时累计量，相邻列可能重叠。AOD 来自 Open-Meteo / CAMS。云图每 10800 秒更新，AOD 失败不影响其他字段，缺失显示 `--`。
- 云图默认直连；需要代理时仅在本地 `config.yaml` 填写 `cloud.proxy`。`cloud.url`、`cloud.air_quality_url` 可覆盖接口地址。
- 新闻每 900 秒更新。设置页可选择新闻平台，也可编辑天气城市、日落城市、刷新间隔、局刷波形和旋转方向。
- 用量 API 可通过 `agent_usage.password` 发送 Bearer 认证；真实认证信息只保存在本地配置。
- 缓存文件默认是 `.cache/dashboard-data.json`；失败时保留上次成功数据。缓存、日志和实时截图不属于公开资料。

设置页读取 Kindle 触摸事件设备，不依赖浏览器。保存配置采用原子替换，保留其他数据源和认证字段；保存结果是 YAML 兼容的 JSON 文本，下次启动继续读取。

## 桌面检查

```sh
cmake -S . -B .build/native -DCMAKE_BUILD_TYPE=Release -DDASHBOARD_WINDOW=ON
cmake --build .build/native -j 4
ctest --test-dir .build/native --output-on-failure
python3 tests/integration.py
```

`tests/integration.py` 仅使用 Python 标准库和本地回环 HTTP 服务，不需要真实接口或认证。桌面窗口依赖 `DISPLAY`；无桌面环境时使用 `-DDASHBOARD_WINDOW=OFF`。

开发用可选依赖：

```sh
python3 -m venv .venv --system-site-packages
.venv/bin/pip install -r requirements-dev.txt
```

`tools/reference_check.py` 是可选的旧参考渲染器对比工具，需要同级目录已有 `kindle-dash-gen` Python 项目。它生成虚构样本，并将 PNG、HTML、差异图和比较结果写入 `preview/qa`。公开源码与正常构建不依赖该同级项目。

字体、图标与交易日历已随 `assets/` 提供。仅在更新素材时运行 `tools/prepare_assets.py`；该工具同样需要本地参考项目。私有 `reference.html` 是可选的本地视觉参考，不纳入 Git。

无网 WSL 的开发备用方案是先在 Windows 执行 `python tools/prepare_native_headers.py`，从上游下载头文件到 `.build/include`。CMake 同时支持系统开发包；这些下载不属于设备运行时依赖。

## ARM 构建与设备

目标 KPW3 需要可运行自定义程序的环境及带图像支持的 FBInk。面板尺寸参考 [FBInk 设备记录](https://www.mobileread.com/forums/showthread.php?p=3873491)，调用参数见 [FBInk CLI](https://github.com/NiLuJe/FBInk/blob/master/CLI.md)。

程序调用 `--image file=...,x=...,y=... --noviewport --wait`；全刷额外使用 `--flash`，局刷不清屏。桌面预览只能验证像素和区域，不能模拟墨水屏波形、残影和电源管理。

现有 ARM 静态构建流程：

```powershell
.\tools\prepare-arm.ps1
python tools/prepare_native_headers.py
wsl -d Ubuntu -- bash tools/build-arm.sh
wsl -d Ubuntu -- bash tools/package-arm.sh
.\tools\deploy-kindle.ps1 -Device kindle.local -Port 2222
```

构建依赖默认保存在 WSL 的 `/var/tmp/kindle-dashboard-arm`，可用 `KINDLE_BUILD_DIR` 指定其他目录。安装包位于 `dist/kindle-dashboard-0.1.0-kpw3.tar.gz`，包含程序、字体、CA 证书、配置和菜单扩展；FBInk 需要预先安装在设备上。

**打包脚本复制本地 `config.yaml`。生成的安装包是私有部署材料，不要作为 GitHub 公开附件发布。** `dist/` 已被 Git 忽略。

部署脚本通过 SSH 上传并校验哈希，停止旧服务，替换程序和菜单扩展，启动后检查进程；失败时尝试恢复旧版本。菜单扩展安装到 `/mnt/us/extensions/kindle-dashboard-native`。仅在明确选择设备地址后运行部署命令。

若使用自己的兼容 ARM SDK，可用 `cmake/kindle.cmake` 指定 `KINDLE_TOOLCHAIN_PREFIX` 和 `KINDLE_SYSROOT`。sysroot 需要 FreeType、HarfBuzz、libpng、libyaml、OpenSSL、libcurl ≥ 7.85 与 nlohmann/json；编译器需支持 C++17 filesystem，不能用普通 Ubuntu ARM 动态库代替 Kindle 运行库。

设备首次运行前可执行 `bin/kindle-dashboard --simulate --offline --once --config tests/demo.yaml --snapshot tests/sample.json` 检查。使用实际数据前还需准备自己的 `config.yaml`。CA 路径可通过 `CURL_CA_BUNDLE=/mnt/us/kindle-dashboard/certs/cacert.pem` 指定；保持设备时间正确，程序仍校验 HTTPS 证书和主机名。

`kindle/control.sh` 在运行期间停止会覆盖屏幕的系统服务并控制休眠，退出时恢复原状态。不同固件的系统服务、休眠和长期运行行为仍需实机验证。

## 本地私有资料

`config.yaml`、`.env*`、`*.local.yaml`、私钥、`.local/`、`reference.html`、`.cache/`、`preview/`、`logs/`、`.build/` 和 `dist/` 均被忽略。`config.example.yaml` 与 `tests/demo.yaml` 是可公开模板；`docs/screenshots/` 只放经审查的虚构数据截图。

程序不会自动读取 `.env`；认证值和私有接口应写入本地 `config.yaml`。保持公共分支不含私有内容，发布前检查暂存文件与历史；不要强制添加被忽略的配置或上传本地安装包。
