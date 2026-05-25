FROM python:3.11-slim
WORKDIR /app

# Proxy configuration (for Orange Pi offline builds via SSH tunnel)
ARG HTTP_PROXY=http://127.0.0.1:18888
ARG HTTPS_PROXY=http://127.0.0.1:18888
ENV http_proxy=$HTTP_PROXY https_proxy=$HTTPS_PROXY HTTP_PROXY=$HTTP_PROXY HTTPS_PROXY=$HTTPS_PROXY

RUN echo 'Acquire::http::Proxy "http://127.0.0.1:18888";' > /etc/apt/apt.conf.d/99proxy && \
    apt-get update && apt-get install -y --no-install-recommends \
    libopenblas0 libgomp1 && \
    rm -rf /var/lib/apt/lists/*

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY src/ ./src/
ENTRYPOINT ["python", "-m", "src", "--config", "config.yaml"]
