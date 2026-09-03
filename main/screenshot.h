#pragma once

// 串口屏幕截图服务(FAP_SCREENSHOT_V1):供 AI Passport 社区发布工具等主机
// 通过 USB 串口读取当前屏幕帧。screenshot_init() 后常驻一个低优先级后台
// 任务,空闲时只做 50ms 周期的非阻塞轮询,对 UI 与音频无影响。
void screenshot_init(void);
