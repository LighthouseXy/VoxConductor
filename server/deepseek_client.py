import json
import os
import urllib.error
import urllib.request


API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-v4-flash"


def ask_deepseek(
    user_text: str,
    history: list[dict[str, str]] | None = None,
) -> str:
    api_key = os.environ.get("DEEPSEEK_API_KEY")

    if not api_key:
        raise RuntimeError(
            "没有找到DEEPSEEK_API_KEY，请先加载server/.env"
        )

    # 第一轮只发送固定文字，单独验证DeepSeek调用链路
    request_body = {
        "model": MODEL,
        "messages": [
            {
                "role": "system",
                "content": (
                    "你是VoxConductor桌面智能语音中枢。"
                    "请结合对话历史理解当前问题。"
                    "如果历史中没有相关信息，不要假装记得。"
                    "请用简短中文回答，不超过两句话，"
                    "适合在10秒内朗读完成。"
                ),
            },

            # 将服务器保存的最近对话放在当前问题之前
            *(history or []),

            {
                "role": "user",
                "content": user_text,
            },
        ],
        "stream": False,
        "max_tokens": 100,

        # 固定文本测试不需要深度思考，减少等待时间和消耗
        "thinking": {
            "type": "disabled",
        },
    }

    request = urllib.request.Request(
        API_URL,
        data=json.dumps(
            request_body,
            ensure_ascii=False,
        ).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )

    print(f"正在调用模型：{MODEL}")

    try:
        with urllib.request.urlopen(
            request,
            timeout=60,
        ) as response:
            response_body = json.load(response)

    except urllib.error.HTTPError as error:
        error_body = error.read().decode(
            "utf-8",
            errors="replace",
        )

        raise RuntimeError(
            f"DeepSeek请求失败：HTTP {error.code}\n{error_body}"
        ) from error

    except urllib.error.URLError as error:
        raise RuntimeError(
            f"无法连接DeepSeek：{error.reason}"
        ) from error

    try:
        answer = response_body["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as error:
        raise RuntimeError(
            "DeepSeek返回格式与预期不符：\n"
            + json.dumps(
                response_body,
                ensure_ascii=False,
                indent=2,
            )
        ) from error

    return answer


def main() -> None:
    answer = ask_deepseek(
        "你好，请用一句话介绍你自己。"
    )

    print("\nDeepSeek回答：")
    print(answer)


if __name__ == "__main__":
    main()
