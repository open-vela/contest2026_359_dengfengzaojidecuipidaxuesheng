# ESPClaw 云端应用商店

FastAPI + SQLite 服务端，与板端应用商店使用同一份 v1 JSON 目录格式。包含公开目录与不可变脚本下载、管理员发布/下架、首页海报配置、浏览器管理页面、持久化数据库与 Docker 启动材料。此目录提供代码，不会自动部署到公网。

## 本机运行

在仓库根目录使用 Python 3.13 或 3.14：

```powershell
python -m venv D:\vela-store-env
D:\vela-store-env\Scripts\python.exe -m pip install -r cloud/app_store/requirements-lock.txt
$env:STORE_DATABASE = "D:\vela-store-data\store.sqlite3"
$env:STORE_ADMIN_TOKEN = "替换为你生成的至少32字节随机令牌"
D:\vela-store-env\Scripts\python.exe -m uvicorn cloud.app_store.app:app --host 127.0.0.1 --port 8080 --no-access-log
```

打开 [本机管理页面](http://127.0.0.1:8080/admin)，输入同一个管理员令牌。令牌仅保留在当前页面输入框，不写浏览器存储、不写板端、不拼入 URL；服务器不打印它。没有设置令牌时公开读接口仍可用，写接口关闭。

也可使用本机管理员启动入口，在仓库外保留令牌并复用数据库：

```powershell
D:\vela-store-env\Scripts\python.exe -m cloud.app_store.local --state D:\vela-store-private --database D:\vela-store-data\store.sqlite3 --port 8080
```

首次生成 `--state` 目录下的 `admin-token.txt`，之后重启沿用同一令牌；选择仅当前用户可访问的私有目录，勿提交令牌。将文件内容填入管理页的「管理员令牌」，点击「连接管理接口」即可发布和保存。需要修改已有应用时，点版本列表中的「编辑并发布新版本」，页面会载入源码与元数据，并为标准三段版本号建议下一个修订号；原版本保留。

可在另一个终端使用同一环境变量发布三个参考应用：

```powershell
D:\vela-store-env\Scripts\python.exe -m cloud.app_store.seed --base http://127.0.0.1:8080
```

参考应用使用独立包名 `store.cloud_calculator` / `store.cloud_focus` / `store.cloud_counter`，保留设备内置应用和原有数据。也可在管理页面选择你自己的单文件 `app.js`，填写元数据后发布。服务端不执行上传脚本；设备安装时再次校验 SHA-256、字节数和真实 QuickJS 语法。

### 发布工作台与实时预览

管理页提供应用发布、首页海报编排和可搜索的版本列表。公开目录在未登录时也会载入预览；发布、下架、恢复上架和保存首页仍需管理员令牌。

- 编辑应用名称、简介、分类或图标时，实时更新应用详情预览。点击已发布应用的「预览」或预览画面中的卡片，可查看该版本的目录信息。
- 「首页与海报」支持可视化编辑标题、副标题，以及最多两张推荐海报的目标应用、文案、配色、装饰和展示数字；可以添加、移除或调整顺序。高级 JSON 与可视化表单双向同步；无效 JSON 会保留上一次可用预览并阻止保存。
- 预览可切换浅色／深色，点击「放大」查看更大的模拟画面，按 Esc 关闭。页面适配电脑双栏与窄屏单栏布局。
- 预览为 1024 × 600 商店目录／海报的浏览器外观模拟，不是 LVGL 逐像素渲染，也不运行上传的 QuickJS 脚本；应用实际行为与安装结果需在设备上验证。
- 表单内容修改后需要明确点击「发布这个版本」或「保存首页配置」才会写入服务端。刷新目录保留未保存的首页草稿；页面重新加载或关闭会清除本页草稿与令牌，不使用浏览器持久化存储。

板子访问电脑时，`127.0.0.1` 指向板子本身。局域网调试应显式选择电脑监听地址/网卡与端口，并在设备中填写电脑的 LAN IPv4 地址，例如 `http://192.168.1.20:8080`。生产服务器使用 HTTPS。此项目不会自动修改防火墙、域名或公网入口。

## 板端接入

在「应用商店 → 云端商城 → 配置地址」填写服务根地址，保存后读取 `/v1/catalog`。地址保存在内置配置 `/data/qpk/.store-cloud.json`；留空明确关闭云端来源。管理员令牌不需要也不应该填到板上。

- HTTPS 校验证书和主机名，使用现有 Sectigo R46 / ISRG Root X1 信任根；不跟随跳转、不接受压缩响应。局域网/回环 IPv4 支持 HTTP 调试，公网明文 HTTP 不接受。
- 下载地址由已校验目录中的 SHA-256 构造为同一服务根下的 `/v1/blobs/<sha256>/app.js`，不能由目录指定其他 URL。30 秒 TLS 总截止时间、5 秒连接截止时间、取消标志及正文容量上限共同约束请求。
- HTTP 明文连接阶段沿用系统客户端 10 秒超时；取消不会阻塞 GUI，但后台完成关闭后才释放工作状态。实际 Wi-Fi/DNS/TLS 的板端耗时需要上板验收。
- 云端断开时内置和 SD 应用可继续使用，已安装应用仍在「全部应用」中。首版不持久缓存云端目录，不自动重试/升级/覆盖已安装包；安装位置始终由用户选择。
- v1 设备目录总上限 24 条，含 3 条内置；服务端最多 21 个上架应用。若与 SD 合并超限或重名，明确报告云端目录错误并保留本地来源，不截断后假装加载完整。
- SD 中一旦显式设置任何首页字段，整套 SD 首页优先（未填字段沿用内置默认）；未自定义 SD 首页时，云端发布的推荐、标题和样式可以直接生效。海报按包名解析，不能打开固定列表下标。

## 接口

| 方法 | 路径 | 权限 | 行为 |
|---|---|---|---|
| GET | `/healthz` | 公开 | 数据库与 schema 状态 |
| GET | `/v1/catalog` | 公开 | v1 目录、首页与海报；ETag / 304 |
| GET | `/v1/blobs/{sha256}/app.js` | 公开 | 不可变 UTF-8 脚本；长度、ETag 与 immutable 缓存策略 |
| GET | `/v1/admin/apps` | Bearer | 版本记录与首页配置 |
| POST | `/v1/admin/apps` | Bearer | 发布完整元数据和 `source`，服务端计算 SHA 与 bytes |
| DELETE | `/v1/admin/apps/{package}` | Bearer | 下架并移除相关推荐，保留历史不可变包供已有读取者使用 |
| POST | `/v1/admin/apps/{package}/restore` | Bearer | 明确重新上架当前版本，不恢复已移除的推荐 |
| PUT | `/v1/admin/home` | Bearer | 修改首页与最多两个推荐 |
| GET | `/admin` | 页面公开，写入须令牌 | 本地发布管理界面 |
| GET | `/openapi.json` | 公开 | 机器可读 OpenAPI schema |

发布请求示例：

```json
{
  "package": "store.my_tool",
  "name": "我的工具",
  "version": "1.0.0",
  "category": 0,
  "icon": "grid",
  "summary": "实际用途说明",
  "source": "ui.sidebar([]); ui.text('Hello',24,24,600,28,ui.primary);"
}
```

名字最多 47 UTF-8 字节，版本 23，简介 191；源码最多 64 KiB，索引最多 24 KiB。包名必须为 `store.` 加小写字母/数字/下划线，不覆盖设备内置包。相同包名、版本、内容重复发布幂等，不回退新版本或撤销下架；修改同版本的脚本或元数据返回 409，须发布新版本。重新上架使用独立操作。服务器保存新版本不等于设备自动升级。

首页字段与样式见 [板端商店文档](../../docs/APP_STORE.md)。数据库使用参数化 SQL、事务和 WAL；版本与脚本以 SHA 对应存储在 SQLite 中，不解压上传目录，也不接受任意文件路径。请求体在 JSON 解析前限制为 512 KiB，兼容转义后的 64 KiB 源码。v1 无付费、用户账户或下载次数统计。

## Docker 与 HTTPS

```sh
# 仓库根目录，先在环境中配置 STORE_ADMIN_TOKEN
docker compose -f cloud/app_store/compose.yaml up -d --build
```

容器使用非 root 用户，数据库卷持久化，默认只映射主机 `127.0.0.1:8080`。公网使用自己的域名和 HTTPS 反向代理；[nginx.example.conf](nginx.example.conf) 是待填域名/证书路径的示例，不会申请证书或发布服务。未在当前环境实际构建 Docker 镜像时，不能把此配方称为容器部署验收。

## 测试

```sh
python -m pip install -r cloud/app_store/requirements-dev.txt
python -m pytest cloud/app_store/test_app.py -q
python tools/test_store_cloud_loopback.py
python tools/export_store_cloud_contract.py --output /path/to/cloud-contract
python3 tools/test_store_http.py
python3 tools/test_app_store.py --apps /path/to/prepared/apps \
  --quickjs-cache /path/to/quickjs-asan-objects --cloud-fixture /path/to/cloud-contract
```

测试覆盖鉴权、版本不可变、真实下载摘要、首页发布、下架、超限、并发、重启、数据保留、C 客户端协议策略与同一 API 响应驱动的安装器。ASGI 测试使用真实 SQLite 和 [FastAPI 的 lifespan / TestClient 流程](https://fastapi.tiangolo.com/advanced/testing-events/)；网络回调替身不代表真实 P4 Wi-Fi/TLS 已验收。
