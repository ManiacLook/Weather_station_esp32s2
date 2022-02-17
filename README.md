[toc]

# 1.功能

​	本地气象台，在屏幕上显示时间和天气信息。

# 2.实现

## 2.1.连接互联网

​	`esp32` 连接互联网的步骤大致可以分为：

- 配置 `wifi` 连接参数
- 注册 `wifi` 连接事件
- 完成 `wifi` 连接事件

具体参考官方例程 `examples\wifi\getting_started\station` 和 [官方文档](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/api-guides/wifi.html#esp32-wi-fi-station) 。

## 2.2.通过 http 协议获取天气数据

​	通过流的方法：

- 通过 `esp_http_client_config_t` 结构体定义 http 的参数
- 通过 `esp_http_client_init()` 进行初始化
- 通过 `esp_http_client_set_method()` 设置发送get请求
- 通过 `esp_http_client_open()` 与目标主机建立连接，发送请求
- 通过 `esp_http_client_fetch_headers()` 获取目标主机的 `response` 报文的头信息，判断是否成功获取数据
- 通过 `esp_http_client_read_response()` 获取报文的返回数据内容

具体参考 [CSDN 博客](https://blog.csdn.net/qq_41741344/article/details/118613219) 。

## 2.3.获得实时时间

​	参考 [官方文档_System Time](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s2/api-reference/system/system_time.html) 和 [官方博客_时间同步](https://blog.csdn.net/espressif/article/details/103001337) 。

## 2.4.LCD 屏幕显示

​	配置 `SPI` 驱动 `LCD` 屏幕显示，参考 [该项目](https://gitee.com/dong-xiwei/esp) 中的 `LCD` 部分代码和官方例程 `examples\peripherals\spi_master\lcd` 。

















