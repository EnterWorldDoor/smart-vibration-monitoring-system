.PHONY: build test run clean

build:
	go build -o edge-router ./cmd/edge-router

test:
	go test ./internal/...

run:
	go run ./cmd/edge-router -config ../../config/edge-router.yaml

clean:
	rm -f edge-router
