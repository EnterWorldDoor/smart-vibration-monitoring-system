FROM golang:1.23-alpine AS builder
ARG HTTP_PROXY
ARG HTTPS_PROXY
ENV HTTP_PROXY=${HTTP_PROXY} HTTPS_PROXY=${HTTPS_PROXY}
ENV GOTOOLCHAIN=auto
WORKDIR /app
COPY go.mod ./
RUN go mod download
COPY . .
RUN go mod tidy && CGO_ENABLED=0 go build -ldflags="-s -w" -o edge-router ./cmd/edge-router

FROM alpine:3.19
WORKDIR /app
COPY --from=builder /app/edge-router .
ENTRYPOINT ["./edge-router"]
