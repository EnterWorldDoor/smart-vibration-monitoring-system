package db

import "testing"

func TestBuildSeverityFilter(t *testing.T) {
	tests := []struct {
		input    string
		expected string
	}{
		{"", ""},
		{"  ", ""},
		{"normal", "NORMAL"},
		{"NORMAL", "NORMAL"},
		{"warning", "WARNING"},
		{"WARNING", "WARNING"},
		{"critical", "CRITICAL"},
		{"CRITICAL", "CRITICAL"},
		{"invalid", ""},
		{"N0rmal", ""},
		{"  WARNING  ", "WARNING"},
	}

	for _, tt := range tests {
		result := BuildSeverityFilter(tt.input)
		if result != tt.expected {
			t.Errorf("BuildSeverityFilter(%q) = %q, want %q", tt.input, result, tt.expected)
		}
	}
}
