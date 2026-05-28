.PHONY: build test run clean

build:
	go build -o server ./cmd/server

test:
	go test ./internal/...

run:
	go run ./cmd/server -config ../../config/ota-server.yaml

clean:
	rm -f server
