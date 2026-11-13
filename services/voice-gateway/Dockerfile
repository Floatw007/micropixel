FROM python:3.12-slim

RUN apt-get update \
    && apt-get install --no-install-recommends -y libopus0 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY pyproject.toml README.md ./
COPY src ./src
RUN pip install --no-cache-dir '.[opus]'
USER nobody
EXPOSE 8080
CMD ["uvicorn", "micropixel_voice_gateway.app:app", "--host", "0.0.0.0", "--port", "8080"]
