# syntax=docker/dockerfile:1

# The server as an image: the locked dependencies in a virtualenv, the modules
# next to it, and `python main.py` as the command, the same as a local run.
# Settings come from the environment (docs/server.md); nothing secret is baked in.

FROM python:3.12-slim AS build

COPY --from=ghcr.io/astral-sh/uv:0.12 /uv /bin/uv

# Compiled up front, so the first request does not pay for it. Copy, because
# the cache mount is a different filesystem and cannot be hard-linked from.
ENV UV_COMPILE_BYTECODE=1 \
    UV_LINK_MODE=copy \
    UV_PYTHON_DOWNLOADS=never

WORKDIR /app
# --locked fails the build if uv.lock no longer matches pyproject.toml.
RUN --mount=type=cache,target=/root/.cache/uv \
    --mount=type=bind,source=pyproject.toml,target=pyproject.toml \
    --mount=type=bind,source=uv.lock,target=uv.lock \
    uv sync --locked --no-install-project --no-dev


FROM python:3.12-slim

RUN useradd --system --no-create-home assistant

WORKDIR /app
COPY --from=build /app/.venv /app/.venv
COPY *.py ./
COPY fonts/ ./fonts/

# Unbuffered, because the log is print() and docker has no terminal to flush it.
ENV PATH="/app/.venv/bin:$PATH" \
    PYTHONUNBUFFERED=1

USER assistant
EXPOSE 8000
CMD ["python", "main.py"]
