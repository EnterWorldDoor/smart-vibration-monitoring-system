-- EdgeVib Dashboard Phase 2: PostgreSQL VIEWs
-- Extracts JSONB payload fields into flat columns for Grafana querying.
-- Import: docker exec -i edgevib-timescaledb psql -U edgevib -d edgevib_ts < views.sql

-- ============================================================================
-- vibration_view: RMS values, peak frequency/amplitude, FFT peaks array
-- ============================================================================
CREATE OR REPLACE VIEW vibration_view AS
SELECT
    time,
    site_id,
    device_type,
    device_id,
    (payload #>> '{data,vibration,rms_x}')::float      AS rms_x,
    (payload #>> '{data,vibration,rms_y}')::float      AS rms_y,
    (payload #>> '{data,vibration,rms_z}')::float      AS rms_z,
    (payload #>> '{data,vibration,overall_rms}')::float AS overall_rms,
    (payload #>> '{data,vibration,peak_freq}')::float  AS peak_frequency_hz,
    (payload #>> '{data,vibration,peak_amp}')::float   AS peak_amplitude_g,
    payload -> 'data' -> 'fft_peaks'                    AS fft_peaks
FROM sensor_data
WHERE payload #> '{data,vibration}' IS NOT NULL
  AND source_path = 'mqtt';

-- ============================================================================
-- ai_diagnosis_view: AI classification results, confidence, cascade source
-- ============================================================================
CREATE OR REPLACE VIEW ai_diagnosis_view AS
SELECT
    time,
    site_id,
    device_type,
    device_id,
    (payload #>> '{data,ai,class_id}')::int               AS ai_class_id,
    payload #>> '{data,ai,class_name}'                     AS ai_class_name,
    (payload #>> '{data,ai,confidence}')::float            AS ai_confidence,
    payload #>> '{data,ai,cascade_source}'                 AS ai_cascade_source,
    (payload #>> '{data,ai,inference_time_us}')::int       AS ai_inference_time_us
FROM sensor_data
WHERE payload #> '{data,ai}' IS NOT NULL
  AND source_path = 'mqtt';

-- ============================================================================
-- dual_channel_view: DE/NDE comparison metrics (only when NDE is online)
-- ============================================================================
CREATE OR REPLACE VIEW dual_channel_view AS
SELECT
    time,
    site_id,
    device_type,
    device_id,
    (payload #>> '{data,dual_channel,rms_ratio}')::float             AS rms_ratio,
    (payload #>> '{data,dual_channel,spectral_similarity}')::float   AS spectral_similarity,
    (payload #>> '{data,dual_channel,phase_coherence}')::float       AS phase_coherence,
    (payload #>> '{data,dual_channel,nde_online}')::int              AS nde_online,
    (payload #>> '{data,dual_channel,nde_errors}')::int              AS nde_errors
FROM sensor_data
WHERE payload #> '{data,dual_channel}' IS NOT NULL
  AND source_path = 'mqtt';

-- ============================================================================
-- environment_view: temperature, humidity, compensation status
-- ============================================================================
CREATE OR REPLACE VIEW environment_view AS
SELECT
    time,
    site_id,
    device_type,
    device_id,
    (payload #>> '{data,environment,temperature_c}')::float      AS temperature_c,
    (payload #>> '{data,environment,humidity_rh}')::float        AS humidity_rh,
    COALESCE(
        (payload #>> '{data,compensation,active}')::boolean,
        false
    )                                                            AS compensation_active
FROM sensor_data
WHERE payload #> '{data,environment}' IS NOT NULL
  AND source_path = 'mqtt';

-- ============================================================================
-- device_status_view: latest status per device (one row per device)
-- ============================================================================
CREATE OR REPLACE VIEW device_status_view AS
WITH latest AS (
    SELECT
        *,
        ROW_NUMBER() OVER (
            PARTITION BY site_id, device_type, device_id
            ORDER BY time DESC
        ) AS rn
    FROM sensor_data
    WHERE source_path = 'mqtt'
)
SELECT
    time                                                  AS last_seen,
    site_id,
    device_type,
    device_id,
    payload #>> '{service_state}'                         AS service_state,
    (payload #>> '{data_quality}')::int                    AS data_quality,
    (payload #>> '{data,vibration,overall_rms}')::float    AS last_rms,
    (payload #>> '{data,environment,temperature_c}')::float AS last_temperature,
    payload #>> '{data,ai,class_name}'                     AS last_ai_class,
    (payload #>> '{data,ai,confidence}')::float            AS last_ai_confidence,
    (payload #>> '{samples_analyzed}')::int                AS samples_analyzed,
    (payload #>> '{total_analyses}')::int                  AS total_analyses,
    (payload #>> '{dev_id}')::int                          AS esp32_dev_id
FROM latest
WHERE rn = 1;
