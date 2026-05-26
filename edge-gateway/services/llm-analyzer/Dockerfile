FROM python:3.11-slim
WORKDIR /app

# Build dependencies for llama-cpp-python C++ compilation
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libopenblas0 \
    libopenblas-dev \
    && rm -rf /var/lib/apt/lists/*

ENV CMAKE_ARGS="-DLLAMA_BLAS=ON -DLLAMA_BLAS_VENDOR=OpenBLAS"

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Clean up build deps to reduce image size, but keep runtime libs needed by llama.cpp
RUN apt-mark manual libgomp1 libopenblas0 && \
    apt-get remove -y build-essential libopenblas-dev && \
    apt-get autoremove -y && \
    rm -rf /var/lib/apt/lists/*

COPY src/ ./src/
COPY prompts/ ./prompts/

ENTRYPOINT ["python", "-m", "src", "--config", "config.yaml"]
