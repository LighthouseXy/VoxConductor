#pragma once

// 复制为 cloud_credentials.h 后填写，或使用 tools/import_cloud_config.py。
// cloud_credentials.h 已被 Git 忽略；烧录后的固件包含密钥，不要公开二进制。
#define CLOUD_DASHSCOPE_API_KEY ""
#define CLOUD_DEEPSEEK_API_KEY ""
#define CLOUD_ASR_URL "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
#define CLOUD_ASR_MODEL "qwen3-asr-flash"
#define CLOUD_ASR_REALTIME_URL "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime"
#define CLOUD_DEEPSEEK_URL "https://api.deepseek.com/chat/completions"
#define CLOUD_DEEPSEEK_MODEL "deepseek-flash"
#define CLOUD_TTS_URL "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-flash-realtime"
#define CLOUD_TTS_VOICE "Cherry"
