-- EdgeVib TimescaleDB Initialization
-- Creates hypertable for sensor time-series data

CREATE TABLE IF NOT EXISTS sensor_data (
    time        TIMESTAMPTZ NOT NULL,
    site_id     TEXT NOT NULL,
    device_type TEXT NOT NULL,
    device_id   TEXT NOT NULL,
    data_type   TEXT NOT NULL,
    payload     JSONB NOT NULL,
    source_path TEXT NOT NULL DEFAULT 'mqtt',  -- 'mqtt' | 'rs232' | 'ethernet'
    ingested_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Convert to hypertable (TimescaleDB time-series optimization)
SELECT create_hypertable('sensor_data', 'time', if_not_exists => TRUE);

-- Indexes for common queries
CREATE INDEX IF NOT EXISTS idx_sensor_device_time
    ON sensor_data (site_id, device_id, time DESC);

CREATE INDEX IF NOT EXISTS idx_sensor_type_time
    ON sensor_data (data_type, time DESC);

CREATE INDEX IF NOT EXISTS idx_sensor_source
    ON sensor_data (source_path);

-- Retention policy: auto-drop data older than 90 days (adjust as needed)
SELECT add_retention_policy('sensor_data', INTERVAL '90 days', if_not_exists => TRUE);

-- AI analysis results table
CREATE TABLE IF NOT EXISTS ai_reports (
    time               TIMESTAMPTZ NOT NULL,
    site_id            TEXT NOT NULL,
    report_type        TEXT NOT NULL,              -- 'fault_diagnosis' | 'trend_prediction' | 'rul' | 'llm_summary' | 'anomaly_detection' | 'motor_health'
    device_id          TEXT,
    severity           TEXT,                       -- 'NORMAL' | 'WARNING' | 'CRITICAL'
    payload            JSONB NOT NULL,
    model_name         TEXT,
    model_version      TEXT,
    anomaly_score      DOUBLE PRECISION,           -- Autoencoder reconstruction MSE
    health_score       DOUBLE PRECISION,           -- Motor comprehensive health score (0-100)
    inference_time_ms  DOUBLE PRECISION,           -- Model inference time in ms
    details            JSONB,                      -- Supplementary details (trends, ratio, context)
    created_at         TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

SELECT create_hypertable('ai_reports', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS idx_ai_reports_severity
    ON ai_reports (severity, time DESC);

-- LLM-generated fault reports table
CREATE TABLE IF NOT EXISTS llm_reports (
    time                TIMESTAMPTZ NOT NULL,
    site_id             TEXT NOT NULL,
    device_id           TEXT,
    report_type         TEXT NOT NULL,              -- 'alert_report' | 'daily_summary'
    severity            TEXT,                       -- 'NORMAL' | 'WARNING' | 'CRITICAL'
    title               TEXT,
    summary             TEXT,
    analysis            TEXT,
    advice              TEXT,
    raw_output          TEXT,                       -- Full LLM output for audit/debug
    model_name          TEXT,
    model_version       TEXT,
    tokens_used         INT,
    generation_time_ms  DOUBLE PRECISION,
    trigger_reason      TEXT,                       -- 'ai_bearing_fault' | 'rms_high' | 'scheduled'
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

SELECT create_hypertable('llm_reports', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS idx_llm_reports_type
    ON llm_reports (report_type, time DESC);

CREATE INDEX IF NOT EXISTS idx_llm_reports_device
    ON llm_reports (site_id, device_id, time DESC);

-- Vision capture metadata table (vision-service)
CREATE TABLE IF NOT EXISTS vision_captures (
    time              TIMESTAMPTZ NOT NULL,
    site_id           TEXT NOT NULL,
    device_id         TEXT NOT NULL,
    capture_type      TEXT NOT NULL,        -- 'baseline' | 'event'
    trigger_src       TEXT,                 -- 'timer' | 'mqtt_inference'
    resolution        TEXT,                 -- '640x480' | '1920x1080'
    file_path         TEXT NOT NULL,
    file_size_bytes   BIGINT
);

SELECT create_hypertable('vision_captures', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS idx_vision_site_device_time
    ON vision_captures (site_id, device_id, time DESC);

SELECT add_retention_policy('vision_captures', INTERVAL '60 days', if_not_exists => TRUE);

-- Audio monitoring: high-frequency acoustic features (~8 rows/s per device)
CREATE TABLE IF NOT EXISTS audio_features (
    time                 TIMESTAMPTZ NOT NULL,
    site_id              TEXT NOT NULL,
    device_id            TEXT NOT NULL,
    rms_energy           DOUBLE PRECISION,
    spectral_centroid_hz DOUBLE PRECISION,
    spectral_kurtosis    DOUBLE PRECISION,
    hf_lf_ratio          DOUBLE PRECISION,
    dominant_freq_hz     DOUBLE PRECISION,
    dominant_amp_db      DOUBLE PRECISION,
    feature_vector       JSONB          -- 128-bin log-spaced downsampled spectrum
);

SELECT create_hypertable('audio_features', 'time',
    chunk_time_interval => INTERVAL '7 days',
    if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS idx_audio_features_device_time
    ON audio_features (site_id, device_id, time DESC);

SELECT add_retention_policy('audio_features', INTERVAL '60 days',
    if_not_exists => TRUE);

-- Audio monitoring: anomaly event records
CREATE TABLE IF NOT EXISTS audio_anomalies (
    time            TIMESTAMPTZ NOT NULL,
    site_id         TEXT NOT NULL,
    device_id       TEXT NOT NULL,
    severity        TEXT NOT NULL,         -- 'warning' | 'critical'
    trigger_reason  TEXT,                  -- 'rms_energy_exceeded' | 'kurtosis_exceeded' | ...
    rms_energy      DOUBLE PRECISION,
    baseline_rms    DOUBLE PRECISION,
    sigma_level     DOUBLE PRECISION,
    wav_path        TEXT,
    duration_ms     INTEGER,
    metadata        JSONB                  -- extra context (spectral_centroid, kurtosis, etc.)
);

SELECT create_hypertable('audio_anomalies', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS idx_audio_anomalies_device
    ON audio_anomalies (site_id, device_id, time DESC);

CREATE INDEX IF NOT EXISTS idx_audio_anomalies_severity
    ON audio_anomalies (severity, time DESC);
