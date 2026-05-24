-- EdgeVib Dashboard Phase 2: Test Data Insert
-- Inserts realistic ESP32 JSON payloads across multiple devices.
-- Run: docker exec -i edgevib-timescaledb psql -U edgevib -d edgevib_ts < insert_test_data.sql

-- Device de01: Normal operation with complete data (vibration + AI + dual_channel)
INSERT INTO sensor_data (time, site_id, device_type, device_id, data_type, payload, source_path)
SELECT
    NOW() - (i * INTERVAL '20 seconds'),
    'factory1', 'motor', 'de01', 'sensor',
    jsonb_build_object(
        'dev_id', 1,
        'timestamp_ms', (EXTRACT(EPOCH FROM NOW() - (i * INTERVAL '20 seconds')) * 1000)::bigint,
        'mode', 'upload',
        'service_state', 'RUNNING',
        'data_quality', 2,
        'samples_analyzed', 2048,
        'total_analyses', i,
        'temperature_valid', 'true',
        'data', jsonb_build_object(
            'vibration', jsonb_build_object(
                'rms_x', 1.0 + random() * 0.5,
                'rms_y', 0.7 + random() * 0.4,
                'rms_z', 1.8 + random() * 0.7,
                'overall_rms', 2.0 + random() * 1.0,
                'peak_freq', 140 + random() * 10,
                'peak_amp', 0.02 + random() * 0.03
            ),
            'environment', jsonb_build_object(
                'temperature_c', 30 + random() * 8,
                'humidity_rh', 60 + random() * 15
            ),
            'compensation', jsonb_build_object(
                'active', 'true', 'offset_x', 0.01, 'offset_y', 0.02, 'offset_z', 0.01
            ),
            'fft_peaks', jsonb_build_array(
                jsonb_build_object('freq', 50.0, 'amp', 0.10 + random() * 0.05),
                jsonb_build_object('freq', 142.0 + random() * 5, 'amp', 0.03 + random() * 0.02),
                jsonb_build_object('freq', 285.0 + random() * 10, 'amp', 0.01 + random() * 0.02)
            ),
            'ai', jsonb_build_object(
                'class_id', CASE WHEN random() < 0.85 THEN 0 ELSE 1 END,
                'class_name', CASE WHEN random() < 0.85 THEN 'normal' ELSE 'imbalance' END,
                'confidence', 0.85 + random() * 0.15,
                'cascade_source', 'primary_cnn',
                'inference_time_us', (12000 + random() * 8000)::int
            ),
            'dual_channel', jsonb_build_object(
                'rms_ratio', 0.9 + random() * 0.4,
                'spectral_similarity', 0.80 + random() * 0.2,
                'phase_coherence', 0.65 + random() * 0.3,
                'nde_online', 1, 'nde_errors', 0
            )
        )
    ) ,
    'mqtt'
FROM generate_series(0, 14) AS i;

-- Device nde01: Second motor bearing (NDE side), no dual_channel
INSERT INTO sensor_data (time, site_id, device_type, device_id, data_type, payload, source_path)
SELECT
    NOW() - (i * INTERVAL '20 seconds') + INTERVAL '5 seconds',
    'factory1', 'motor', 'nde01', 'sensor',
    jsonb_build_object(
        'dev_id', 2,
        'timestamp_ms', (EXTRACT(EPOCH FROM NOW() - (i * INTERVAL '20 seconds')) * 1000)::bigint,
        'mode', 'upload',
        'service_state', 'RUNNING',
        'data_quality', 2,
        'samples_analyzed', 2048,
        'total_analyses', i,
        'temperature_valid', 'true',
        'data', jsonb_build_object(
            'vibration', jsonb_build_object(
                'rms_x', 0.8 + random() * 0.4,
                'rms_y', 0.6 + random() * 0.3,
                'rms_z', 1.5 + random() * 0.6,
                'overall_rms', 1.7 + random() * 0.9,
                'peak_freq', 130 + random() * 15,
                'peak_amp', 0.015 + random() * 0.025
            ),
            'environment', jsonb_build_object(
                'temperature_c', 28 + random() * 6,
                'humidity_rh', 58 + random() * 12
            ),
            'compensation', jsonb_build_object(
                'active', 'true', 'offset_x', 0.01, 'offset_y', 0.01, 'offset_z', 0.02
            ),
            'fft_peaks', jsonb_build_array(
                jsonb_build_object('freq', 50.0, 'amp', 0.08 + random() * 0.04),
                jsonb_build_object('freq', 135.0, 'amp', 0.02 + random() * 0.02)
            )
        )
    ) ,
    'mqtt'
FROM generate_series(0, 14) AS i;

-- Device esp32-01: Gateway status data (no vibration, just environment)
INSERT INTO sensor_data (time, site_id, device_type, device_id, data_type, payload, source_path)
SELECT
    NOW() - (i * INTERVAL '60 seconds'),
    'factory1', 'gateway', 'esp32-01', 'status',
    jsonb_build_object(
        'dev_id', 100,
        'timestamp_ms', (EXTRACT(EPOCH FROM NOW() - (i * INTERVAL '60 seconds')) * 1000)::bigint,
        'mode', 'upload',
        'service_state', 'RUNNING',
        'data_quality', 2,
        'samples_analyzed', 0,
        'total_analyses', i,
        'temperature_valid', 'true',
        'data', jsonb_build_object(
            'environment', jsonb_build_object(
                'temperature_c', 35 + random() * 5,
                'humidity_rh', 45 + random() * 10
            )
        )
    ) ,
    'mqtt'
FROM generate_series(0, 5) AS i;

-- One CRITICAL record: de01 in ERROR with high RMS + bearing_fault
INSERT INTO sensor_data (time, site_id, device_type, device_id, data_type, payload, source_path)
VALUES (
    NOW() - INTERVAL '10 seconds',
    'factory1', 'motor', 'de01', 'sensor',
    jsonb_build_object(
        'dev_id', 1,
        'timestamp_ms', (EXTRACT(EPOCH FROM NOW()) * 1000)::bigint,
        'mode', 'upload',
        'service_state', 'ERROR',
        'data_quality', 0,
        'samples_analyzed', 1024,
        'total_analyses', 200,
        'temperature_valid', 'true',
        'data', jsonb_build_object(
            'vibration', jsonb_build_object(
                'rms_x', 5.2, 'rms_y', 4.8, 'rms_z', 6.1,
                'overall_rms', 8.5, 'peak_freq', 280.0, 'peak_amp', 0.15
            ),
            'environment', jsonb_build_object(
                'temperature_c', 52.0, 'humidity_rh', 70.0
            ),
            'compensation', jsonb_build_object(
                'active', 'false', 'offset_x', 0.0, 'offset_y', 0.0, 'offset_z', 0.0
            ),
            'fft_peaks', jsonb_build_array(
                jsonb_build_object('freq', 280.0, 'amp', 0.15),
                jsonb_build_object('freq', 560.0, 'amp', 0.08)
            ),
            'ai', jsonb_build_object(
                'class_id', 3, 'class_name', 'bearing_fault', 'confidence', 0.94,
                'cascade_source', 'primary_cnn', 'inference_time_us', 22000
            ),
            'dual_channel', jsonb_build_object(
                'rms_ratio', 2.8, 'spectral_similarity', 0.35,
                'phase_coherence', 0.18, 'nde_online', 1, 'nde_errors', 5
            )
        )
    ) ,
    'mqtt'
);
