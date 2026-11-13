from __future__ import annotations

from collections.abc import AsyncIterator

_BOUNDARIES = frozenset("。！？；.!?;\n")


async def sentence_chunks(source: AsyncIterator[str], maximum: int = 72) -> AsyncIterator[str]:
    pending = ""
    async for delta in source:
        pending += delta
        while pending:
            boundary = next((index + 1 for index, value in enumerate(pending) if value in _BOUNDARIES), 0)
            if boundary == 0 and len(pending) < maximum:
                break
            take = boundary or maximum
            yield pending[:take]
            pending = pending[take:]
    if pending:
        yield pending
