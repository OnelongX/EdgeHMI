#ifndef EDGEHMI_DEFAULT_CONFIG_H
#define EDGEHMI_DEFAULT_CONFIG_H

/* Built-in factory default Chinese industrial HMI config used on fresh empty flash */
static const char EDGEHMI_FACTORY_DEFAULT_JSON[] =
"{"
"\"version\":\"2.4.0\","
"\"device\":{\"name\":\"EdgeHMI 工业控制屏 (出厂引导)\",\"width\":800,\"height\":480,"
"\"theme\":\"dark\",\"poll_interval_ms\":250,\"watchdog_timeout_s\":10,"
"\"idle_dim_s\":180,\"idle_brightness_pct\":15,\"engineer_pin\":\"1234\",\"api_token\":\"edgehmi-secret-2026\"},"
"\"modbus\":{\"port\":1,\"baudrate\":9600,\"tx_pin\":17,\"rx_pin\":18,\"de_pin\":16,\"timeout_ms\":300,"
"\"points\":["
"{\"tag\":\"tank_level\",\"slave\":1,\"fc\":3,\"addr\":100,\"type\":\"u16\",\"scale\":0.1,\"unit\":\"%\",\"rw\":\"r\",\"default\":62.0},"
"{\"tag\":\"motor_curr\",\"slave\":1,\"fc\":3,\"addr\":104,\"type\":\"u16\",\"scale\":0.1,\"unit\":\"A\",\"rw\":\"r\",\"default\":3.6},"
"{\"tag\":\"pump_cmd\",\"slave\":1,\"fc\":1,\"addr\":1,\"type\":\"bool\",\"scale\":1.0,\"rw\":\"rw\",\"default\":0}"
"]},"
"\"rules\":[],"
"\"pages\":[{"
"\"id\":\"boot\",\"title\":\"出厂引导与配网\",\"widgets\":["
"{\"id\":\"t1\",\"type\":\"label\",\"x\":24,\"y\":20,\"w\":480,\"h\":36,\"text\":\"EdgeHMI ESP32-S3 工业边缘控制终端\"},"
"{\"id\":\"led1\",\"type\":\"led\",\"x\":610,\"y\":24,\"w\":24,\"h\":24,\"bind\":\"pump_cmd\",\"color\":\"#10B981\"},"
"{\"id\":\"bd1\",\"type\":\"badge\",\"x\":644,\"y\":20,\"w\":132,\"h\":32,\"bind\":\"pump_cmd\",\"color\":\"#10B981\",\"true_text\":\"运行中\",\"false_text\":\"待机中\"},"
"{\"id\":\"c1\",\"type\":\"card\",\"x\":24,\"y\":72,\"w\":420,\"h\":296,\"text\":\"实时工况监测 (出厂演示)\"},"
"{\"id\":\"g1\",\"type\":\"gauge\",\"x\":54,\"y\":120,\"w\":180,\"h\":180,\"bind\":\"tank_level\",\"unit\":\"%\",\"min\":0,\"max\":100,\"decimals\":1,\"color\":\"#2DD4BF\"},"
"{\"id\":\"l1\",\"type\":\"label\",\"x\":262,\"y\":136,\"w\":150,\"h\":24,\"text\":\"主电机电流\"},"
"{\"id\":\"v1\",\"type\":\"value\",\"x\":262,\"y\":168,\"w\":150,\"h\":40,\"bind\":\"motor_curr\",\"unit\":\"A\",\"decimals\":1,\"color\":\"#F59E0B\"},"
"{\"id\":\"c2\",\"type\":\"card\",\"x\":468,\"y\":72,\"w\":308,\"h\":296,\"text\":\"扫码配网与手机监控\"},"
"{\"id\":\"qr1\",\"type\":\"qrcode\",\"x\":548,\"y\":118,\"w\":148,\"h\":148,\"src\":\"http://192.168.4.1/setup\"},"
"{\"id\":\"l2\",\"type\":\"label\",\"x\":490,\"y\":284,\"w\":264,\"h\":60,\"text\":\"连热点 EdgeHMI-XXXX 扫码配网 / 手机监控 /h5\"},"
"{\"id\":\"b1\",\"type\":\"button\",\"x\":24,\"y\":394,\"w\":170,\"h\":54,\"text\":\"启动水泵\",\"bind\":\"pump_cmd\",\"write_value\":1,\"color\":\"#10B981\"},"
"{\"id\":\"b2\",\"type\":\"button\",\"x\":214,\"y\":394,\"w\":170,\"h\":54,\"text\":\"紧急停泵\",\"bind\":\"pump_cmd\",\"write_value\":0,\"color\":\"#EF4444\",\"confirm\":true,\"pin_required\":true}"
"]}]"
"}";

#endif /* EDGEHMI_DEFAULT_CONFIG_H */
