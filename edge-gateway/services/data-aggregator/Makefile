.PHONY: build test run clean

build:
	go build -o aggregator ./cmd/aggregator

test:
	go test ./internal/...

run:
	go run ./cmd/aggregator -config ../../config/aggregator.yaml

clean:
	rm -f aggregator
