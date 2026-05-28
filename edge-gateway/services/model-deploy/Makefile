.PHONY: build test run clean

build:
	go build -o server ./cmd/server

test:
	go test ./internal/...

run:
	go run ./cmd/server -config ../../config/model-deploy.yaml

clean:
	rm -f server
