/* 
** WiFi station 连接，并且从心知天气获取天气和温度
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "fonts.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_http_client.h"
#include "cJSON.h"

// 定义联网相关的宏
#define EXAMPLE_ESP_WIFI_SSID      "look"               // 账号
#define EXAMPLE_ESP_WIFI_PASS      "123456789"          // 密码
#define EXAMPLE_ESP_MAXIMUM_RETRY  5					// wifi连接失败以后可以重新连接的次数
#define WIFI_CONNECTED_BIT BIT0                         // wifi连接成功标志位
#define WIFI_FAIL_BIT      BIT1							// wifi连接失败标志位

#define MAX_HTTP_OUTPUT_BUFFER 2048                     // HTTP接收数据大小s

// 定义LCD相关的宏
#define PIN_NUM_MISO -1  // 主设备输入，从设备输出
#define PIN_NUM_MOSI 35  // 主设备输出，从设备输入
#define PIN_NUM_CLK  36  // （串行）时钟
#define PIN_NUM_CS   -1  // 片选(没有用到)
#define PIN_NUM_DC   33  // 显示屏D/C引脚，区分命令和数据
#define PIN_NUM_RST  34  // 复位引脚

// 定义联网所需要的变量
static EventGroupHandle_t s_wifi_event_group;   // 事件组，用于对wifi响应结果进行标记
static const char* TAG = "wifi station";        // log标志位
static int s_retry_num = 0;                     // 记录wifi重新连接尝试的次数

// 定义LCD相关的变量
DRAM_ATTR static uint8_t screen[8][128] = {0};  // 显示数据，大小(y,x)=64x128
static spi_device_handle_t spi;                 // SPI句柄

void send_cmd(const uint8_t cmd, spi_device_handle_t spi)
{
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));         // Zero out the transaction
    t.length = 8;                     // Command is 8 bits
    t.tx_buffer = &cmd;               // The data is the cmd itself
    t.user = (void*)0;                // D/C needs to be set to 0
    ret = spi_device_polling_transmit(spi, &t);  // Transmit!
    assert(ret == ESP_OK);            // Should have had no issues.
}

void send_data(spi_device_handle_t spi, const uint8_t *data, int len)
{
    esp_err_t ret;
    spi_transaction_t t;
    if (len == 0) return;               // no need to send anything
    memset(&t, 0, sizeof(t));           // Zero out the transaction
    t.length = len * 8;                 // Len is in bytes, transaction length is in bits.
    t.tx_buffer = data;                 // Data
    t.user = (void*)1;                  // D/C needs to be set to 1
    ret = spi_device_polling_transmit(spi, &t);  // Transmit!
    assert(ret == ESP_OK);              // Should have had no issues.
}

void oled_spi_pre_transfer_callback(spi_transaction_t *t)
{
    int dc = (int)t->user;
    gpio_set_level(PIN_NUM_DC, dc);
}

void oled_refresh()
{
	uint8_t m;
	for (m = 0; m < 8; m ++)
	{
		send_cmd(0xb0 + m, spi);
		send_cmd(0x00, spi);
		send_cmd(0x10, spi);
		send_data(spi, screen[m], 128);
	}
}

void oled_drawpoint(uint8_t x, uint8_t y)
{
	uint8_t i = 0, j = 0;
    x = 127 - x;

	i = y / 8;
	j = y % 8;
	j = 1 << j;
	screen[i][x] |= j;
}

void oled_clearpoint(uint8_t x, uint8_t y)
{
	uint8_t i = 0, j = 0;
    x = 127 - x;

	i = y / 8;
	j = y % 8;
	j = 1 << j;
	screen[i][x] = ~screen[i][x];
	screen[i][x] |= j;
	screen[i][x] = ~screen[i][x];
}

void oled_char(uint8_t x, uint8_t y, char chr,uint8_t size1)
{
	uint8_t i, m, temp, size2, chr1;
	uint8_t y0 = y;

	size2 = (size1 / 8 + ((size1 % 8) ? 1 : 0)) * (size1 / 2);  // 得到字体一个字符对应点阵集所占的字节数
	chr1 = chr - ' ';  // 计算偏移后的值
	for (i = 0; i < size2; i ++)
	{
		if (size1 == 12)  // 调用1206字体
        {
            temp = asc2_1206[chr1][i];
        }
		else if (size1 == 16)  // 调用1608字体
        {
            temp = asc2_1608[chr1][i];
        }
		else return;

        for (m = 0; m < 8; m ++)
        {
            if (temp & 0x80)    oled_drawpoint(x, y);
            else oled_clearpoint(x, y);
            temp <<= 1;
            y --;
            if ((y0 - y) == size1)
            {
                y = y0;
                x ++;
                break;
            }
        }
    }
}

void oled_string(uint8_t x, uint8_t y, char *chr, uint8_t size1)
{
	while ((*chr >= ' ') && (*chr <= '~') && (*chr != '.'))  // 判断是不是非法字符!
	{
		oled_char(x, y, *chr, size1);
		x += size1 / 2;
		if (x > 128 - size1)  // 换行
		{
			x = 0;
			y -= size1;
        }
		chr ++;
    }
}

void oled_init()
{
    // Initialize non-SPI GPIOs
    gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);

    // Reset the display
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(100 / portTICK_RATE_MS);
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(100 / portTICK_RATE_MS);
}

// 将screen数组置零
void oled_clear()
{
	uint8_t i, j;
	for (i = 0; i < 8; i ++)
	    for (j = 0; j < 128; j ++)
	        	screen[i][j] = 0;
	oled_refresh();
}

// 启动LCD
void oled_start()
{
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .miso_io_num=PIN_NUM_MISO,
        .mosi_io_num=PIN_NUM_MOSI,
        .sclk_io_num=PIN_NUM_CLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz=128*8,
    };
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz=20*1000*1000,           //Clock out at 20 MHz
        .mode=0,                                //SPI mode 0
        .spics_io_num=PIN_NUM_CS,               
        .queue_size=1,                          
        .pre_cb=oled_spi_pre_transfer_callback, 
    };
    // Initialize the SPI bus
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    // Attach the oled to the SPI bus
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

	oled_init();

	send_cmd(0x8D, spi);
    send_cmd(0x14, spi);
    send_cmd(0xAF, spi);

    // 初始化配置完成
	char *str = "Connecting to wifi";
	oled_string(10, 30, str, 12);
	oled_refresh();
    vTaskDelay(1000 / portTICK_RATE_MS);
}

/*
** @brief 处理wifi连接和ip分配时候事件的回调函数
*/
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    // 如果是wifi station开始连接事件，就尝试将station连接到AP
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    // 如果是wifi station从AP断连事件
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // 如果没有达到最高尝试次数，继续尝试
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num ++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else  // 如果达到了最高尝试次数，就标记连接失败
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    }
    // 如果是ip获取事件，获取到了ip就打印出来
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);  // 成功获取到了ip，就标记这次wifi连接成功
    }
}

/*
** @brief 用于连接wifi的函数
** @param[in] 无
** @retval 无
** @note 这里wifi连接选项设置了使用nvs，会把每次配置的参数存储在nvs中。因此请查看分区表中是否对nvs分区进行了设置
*/
void wifi_init_sta(void)
{
    // 00 创建wifi事件组
    s_wifi_event_group = xEventGroupCreate();

    /******************** 01 Wi-Fi/LwIP 初始化阶段 ********************/
    // 01-1 创建LWIP核心任务
    ESP_ERROR_CHECK(esp_netif_init());

    // 01-2 创建系统事件任务，并初始化应用程序事件的回调函数
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // 01-3 创建有 TCP/IP 堆栈的默认网络接口实例绑定 station
    esp_netif_create_default_wifi_sta();

    // 01-4 创建wifi驱动程序任务，并初始化wifi驱动程序
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 01-5 注册，用于处理wifi连接的过程中的事件
    esp_event_handler_instance_t instance_any_id;  // 用于处理wifi连接时候的事件的句柄
    esp_event_handler_instance_t instance_got_ip;  // 用于处理ip分配时候产生的事件的句柄
    // 该句柄对wifi连接所有事件都产生响应，连接到event_handler回调函数
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    // 该句柄仅仅处理IP_EVENT事件组中的从AP中获取ip地址事件，连接到event_handler回调函数
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    /******************** 02 WIFI配置阶段 ********************/
    // 02-1 定义wifi配置参数
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    // 02-2 配置station工作模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // 02-3 配置
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /******************** 03 wifi启动阶段 ********************/
    // 03-1 启动wifi驱动程序
    ESP_ERROR_CHECK(esp_wifi_start());  // 会触发回调函数

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    /******************** 输出wifi连接结果 ********************/
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    // 05 事件注销
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

static void http_test_task(void *pvParameters)
{
    // 02-1 定义需要的变量
    char output_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};  // 用于接收通过http协议返回的数据
    int content_length = 0;                            // http协议头的长度
    
    // 02-2 配置http结构体
    // 定义http配置结构体，并且进行清零(避免初始化随机值)
    esp_http_client_config_t config;
    memset(&config, 0, sizeof(config));

    // 向配置结构体内部写入url(心知天气API接口地址)
    static const char *URL = "https://api.seniverse.com/v3/weather/now.json?key=your_api_key&location=Wuhan&language=en&unit=c";
    config.url = URL;

    // 初始化结构体
    esp_http_client_handle_t client = esp_http_client_init(&config);  // 初始化http客户端

    // 设置发送get请求
    esp_http_client_set_method(client, HTTP_METHOD_GET);

    // 02-3 循环通讯
    while(1)
    {
        // 与目标主机创建连接，并且声明写入内容长度为0
        esp_err_t err = esp_http_client_open(client, 0);

        // 连接失败
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        } 
        // 连接成功
        else
        {
            // 读取目标主机的返回内容的协议头长度
            content_length = esp_http_client_fetch_headers(client);

            // 如果协议头长度小于0，说明没有成功读取到
            if (content_length < 0)
            {
                ESP_LOGE(TAG, "HTTP client fetch headers failed");
            }
            // 如果成功读取到了协议头
            else
            {
                // 读取目标主机通过http的响应内容
                int data_read = esp_http_client_read_response(client, output_buffer, MAX_HTTP_OUTPUT_BUFFER);  // 读取到的数据长度
                // 响应成功
                if (data_read >= 0)
                {
                    // 打印响应内容，包括响应状态，响应体长度及其内容
                    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                        esp_http_client_get_status_code(client),				// 获取HTTP响应状态
                        esp_http_client_get_content_length(client));			// 获取响应信息长度
                    printf("data:%s\n", output_buffer);
                    // 对接收到的数据作相应的处理
                    cJSON* root = NULL;                 // 头指针
                    root = cJSON_Parse(output_buffer);  // 解析整段JSON数据
                    // 逐层解析键值对
                    cJSON* cjson_item = cJSON_GetObjectItem(root, "results");
                    cJSON* cjson_results = cJSON_GetArrayItem(cjson_item, 0);
                    cJSON* cjson_now = cJSON_GetObjectItem(cjson_results, "now");
                    cJSON* cjson_temperature = cJSON_GetObjectItem(cjson_now, "temperature");
                    cJSON* cjson_text = cJSON_GetObjectItem(cjson_now, "text");
                
                    printf("weather:%s\n", cjson_text->valuestring);
                    printf("temperature:%s\n", cjson_temperature->valuestring);

                    oled_clear();
                    char str[80];
                    sprintf(str, "temperature:%s", cjson_temperature->valuestring);
                    oled_string(10, 50, str, 12);
                    sprintf(str, "weather:%s", cjson_text->valuestring);
                    oled_string(10, 30, str, 12);
                    oled_refresh();
                    vTaskDelay(1000 / portTICK_RATE_MS);
                }
                // 如果不成功
                else
                {
                    ESP_LOGE(TAG, "Failed to read response");
                }
            }
        }

        // 关闭连接
        esp_http_client_close(client);

        // 延时，因为心知天气免费版本每分钟只能获取20次数据
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 启动LCD
    oled_start();

    // 连wifi
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    // 创建进程，用于处理http通讯
    xTaskCreate(&http_test_task, "http_test_task", 8192, NULL, 5, NULL);
}
