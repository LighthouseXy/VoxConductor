from pathlib import Path
import sqlite3


DATABASE_PATH = Path(__file__).parent / "voxconductor.db"
LEGACY_DATABASE_PATH = Path(__file__).parent / "ambient_desk.db"
MAX_CONTEXT_MESSAGES = 12


def connect_database() -> sqlite3.Connection:
    connection = sqlite3.connect(DATABASE_PATH)
    connection.row_factory = sqlite3.Row
    return connection


def init_database() -> None:
    """创建对话消息表。重复执行不会清空已有内容。"""
    # 首次使用新名称时沿用旧数据库，避免项目改名导致上下文丢失。
    if not DATABASE_PATH.exists() and LEGACY_DATABASE_PATH.exists():
        LEGACY_DATABASE_PATH.replace(DATABASE_PATH)

    with connect_database() as connection:
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id TEXT NOT NULL,
                turn_id TEXT NOT NULL,
                role TEXT NOT NULL
                    CHECK (role IN ('user', 'assistant')),
                content TEXT NOT NULL,
                created_at TEXT NOT NULL
                    DEFAULT CURRENT_TIMESTAMP
            )
            """
        )

        # 将单设备V1的旧会话身份同步到新项目名称。
        connection.execute(
            """
            UPDATE messages
            SET session_id = 'voxconductor-01'
            WHERE session_id = 'ambient-desk-01'
            """
        )

        connection.execute(
            """
            CREATE INDEX IF NOT EXISTS
                idx_messages_session_id
            ON messages(session_id, id)
            """
        )


def load_recent_messages(
    session_id: str,
    limit: int = MAX_CONTEXT_MESSAGES,
) -> list[dict[str, str]]:
    """读取最近若干条消息，并恢复成从旧到新的顺序。"""
    with connect_database() as connection:
        rows = connection.execute(
            """
            SELECT role, content
            FROM (
                SELECT id, role, content
                FROM messages
                WHERE session_id = ?
                ORDER BY id DESC
                LIMIT ?
            )
            ORDER BY id ASC
            """,
            (session_id, limit),
        ).fetchall()

    return [
        {
            "role": row["role"],
            "content": row["content"],
        }
        for row in rows
    ]


def save_turn(
    session_id: str,
    turn_id: str,
    user_text: str,
    assistant_text: str,
) -> None:
    """成功取得AI回答后，原子保存一整轮对话。"""
    with connect_database() as connection:
        connection.executemany(
            """
            INSERT INTO messages (
                session_id,
                turn_id,
                role,
                content
            )
            VALUES (?, ?, ?, ?)
            """,
            [
                (
                    session_id,
                    turn_id,
                    "user",
                    user_text,
                ),
                (
                    session_id,
                    turn_id,
                    "assistant",
                    assistant_text,
                ),
            ],
        )

def load_turn(
    session_id: str,
    turn_id: str,
) -> dict[str, str] | None:
    """读取指定会话中的指定轮次。"""
    with connect_database() as connection:
        rows = connection.execute(
            """
            SELECT role, content
            FROM messages
            WHERE session_id = ? AND turn_id = ?
            ORDER BY id ASC
            """,
            (session_id, turn_id),
        ).fetchall()

    values = {
        row["role"]: row["content"]
        for row in rows
    }

    if "user" not in values or "assistant" not in values:
        return None

    return {
        "transcript": values["user"],
        "answer": values["assistant"],
    }

def clear_session(session_id: str) -> None:
    """清空指定会话，其他设备或会话不受影响。"""
    with connect_database() as connection:
        connection.execute(
            """
            DELETE FROM messages
            WHERE session_id = ?
            """,
            (session_id,),
        )
