#!/usr/bin/env python3
"""
EdgeVib Autoencoder → ONNX Converter + Model Metadata Export.

Converts autoencoder_best.h5 to ONNX format for Orange Pi inference-engine deployment.
Also exports normalization parameters and anomaly threshold to model_metadata.json.

Usage:
    conda activate edgevib-tf
    python edge-ai/deployment/onnx_converter.py

Requires: tensorflow>=2.15, tf2onnx, numpy
"""

import json
import os
import sys
from pathlib import Path

import numpy as np

os.environ['TF_CPP_MIN_LOG_LEVEL'] = '2'

try:
    import tensorflow as tf
    import tf2onnx
except ImportError as e:
    print(f"ERROR: {e}")
    print("Run: pip install tensorflow tf2onnx")
    sys.exit(1)

# Add edge-ai to path for feature scaler access
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from data_pipeline.feature_extractor import FeatureExtractor, FeatureConfig

MODEL_DIR = Path(__file__).resolve().parent / "models"
H5_PATH = Path(__file__).resolve().parents[1] / "models" / "saved_models" / "autoencoder_24.h5"
ONNX_OUT = MODEL_DIR / "autoencoder.onnx"
METADATA_OUT = MODEL_DIR / "autoencoder_metadata.json"

# 24-dim feature names (must match ai_service.c push_feature_vector layout)
FEATURE_NAMES = [
    "rms_x", "rms_y", "rms_z", "overall_rms",
    "peak_freq_x", "peak_amp_x", "skewness_x", "kurtosis_x", "crest_factor_x",
    "band_energy_x_0", "band_energy_x_1", "band_energy_x_2", "band_energy_x_3",
    "band_energy_x_4", "band_energy_x_5", "band_energy_x_6", "band_energy_x_7",
    "peak_freq_y", "peak_amp_y", "crest_factor_y",
    "peak_freq_z", "peak_amp_z", "crest_factor_z",
    "temperature_c"
]

# Domain-knowledge defaults for feature normalization (per-feature mean/std).
# These are approximate for industrial vibration (g units). When real training
# data is available, these should be replaced by fit_scaler values.
DEFAULT_NORM_MEAN = [
    0.005, 0.005, 0.005, 0.01,     # rms_x/y/z, overall_rms (~5-10 mg)
    120.0, 0.02, 0.0, 3.0, 3.5,    # peak_freq(Hz), peak_amp(g), skew, kurt, crest
    0.12, 0.12, 0.12, 0.12,        # band_energy_x[0..3] (normalized to 12.5% each)
    0.12, 0.12, 0.12, 0.12,        # band_energy_x[4..7]
    120.0, 0.015, 3.5,             # peak_freq_y, peak_amp_y, crest_factor_y
    120.0, 0.015, 3.5,             # peak_freq_z, peak_amp_z, crest_factor_z
    30.0                            # temperature_c
]

DEFAULT_NORM_STD = [
    0.02, 0.02, 0.02, 0.04,        # rms (wide for fault detection)
    80.0, 0.03, 1.5, 3.0, 2.5,     # peak_freq, peak_amp, skew, kurt, crest
    0.05, 0.05, 0.05, 0.05,        # band energies (varies by machine)
    0.05, 0.05, 0.05, 0.05,
    80.0, 0.02, 2.5,
    80.0, 0.02, 2.5,
    10.0                            # temperature
]

# Default anomaly threshold (MSE). Override after running on validation data.
DEFAULT_ANOMALY_THRESHOLD = 0.15


def load_keras_model(path: Path) -> tf.keras.Model:
    print(f"Loading Keras model: {path}")
    model = tf.keras.models.load_model(str(path), compile=False)
    model.summary()
    return model


def load_scaler_params():
    """Try to load scaler params from training artifacts. Falls back to defaults."""
    scaler_path = Path(__file__).resolve().parents[1] / "models" / "scaler_params.json"
    if scaler_path.exists():
        with open(scaler_path) as f:
            params = json.load(f)
        print(f"Loaded scaler params: {scaler_path} ({len(params['mean'])} features)")
        return params["mean"], params["std"]

    print("WARNING: No scaler_params.json found, using domain-default normalization.")
    print("For production, export scaler params from FeatureExtractor.fit_scaler()")
    return DEFAULT_NORM_MEAN, DEFAULT_NORM_STD


def compute_threshold(model, X_sample, percentile=95.0):
    """Compute anomaly threshold from a sample dataset."""
    print(f"Computing anomaly threshold (p{percentile})...")
    X_recon = model.predict(X_sample, verbose=1)
    mse = np.mean(np.square(X_sample - X_recon), axis=1)
    threshold = float(np.percentile(mse, percentile))
    print(f"  MSE stats: min={mse.min():.6f}, mean={mse.mean():.6f}, "
          f"median={float(np.median(mse)):.6f}, max={mse.max():.6f}")
    print(f"  Threshold (p{percentile}): {threshold:.6f}")
    return threshold, mse


def convert_to_onnx(model: tf.keras.Model) -> bytes:
    print("Converting Keras → ONNX...")
    spec = (tf.TensorSpec(model.input_shape, tf.float32, name="features"),)
    model_proto, _ = tf2onnx.convert.from_keras(model, input_signature=spec, opset=13)
    print(f"  ONNX model size: {len(model_proto.SerializeToString()) / 1024:.1f} KB")
    return model_proto.SerializeToString()


def main():
    model = load_keras_model(H5_PATH)
    actual_input_dim = model.input_shape[-1]
    print(f"Model input dimension: {actual_input_dim}")

    norm_mean, norm_std = load_scaler_params()

    # Slice or pad normalization params to match actual model input dim
    if len(norm_mean) < actual_input_dim:
        print(f"WARNING: norm params ({len(norm_mean)}) < model dim ({actual_input_dim}), padding with zeros")
        norm_mean = list(norm_mean) + [0.0] * (actual_input_dim - len(norm_mean))
        norm_std = list(norm_std) + [1.0] * (actual_input_dim - len(norm_std))
    elif len(norm_mean) > actual_input_dim:
        norm_mean = list(norm_mean[:actual_input_dim])
        norm_std = list(norm_std[:actual_input_dim])

    norm_mean_arr = np.array(norm_mean, dtype=np.float32)
    norm_std_arr = np.array(norm_std, dtype=np.float32)

    # Generate feature names for the actual dimension
    if actual_input_dim == 24:
        feature_names = FEATURE_NAMES
    else:
        feature_names = [f"feature_{i}" for i in range(actual_input_dim)]
        if actual_input_dim >= 24:
            for i in range(min(24, actual_input_dim)):
                feature_names[i] = FEATURE_NAMES[i] if i < len(FEATURE_NAMES) else feature_names[i]

    # Compute anomaly threshold
    rng = np.random.default_rng(42)
    X_cal = rng.normal(
        loc=norm_mean_arr[:actual_input_dim],
        scale=norm_std_arr[:actual_input_dim] * 0.3,
        size=(500, actual_input_dim)
    ).astype(np.float32)

    X_cal_norm = (X_cal - norm_mean_arr[:actual_input_dim]) / np.maximum(norm_std_arr[:actual_input_dim], 1e-10)

    threshold, mse_samples = compute_threshold(model, X_cal_norm)
    if threshold <= 0:
        threshold = DEFAULT_ANOMALY_THRESHOLD
        print(f"WARNING: Computed threshold is 0, using default: {threshold}")

    # Convert to ONNX
    onnx_bytes = convert_to_onnx(model)

    # Save ONNX model
    MODEL_DIR.mkdir(parents=True, exist_ok=True)
    with open(ONNX_OUT, "wb") as f:
        f.write(onnx_bytes)
    print(f"ONNX model saved: {ONNX_OUT} ({len(onnx_bytes) / 1024:.1f} KB)")

    # Build metadata
    input_shape = list(model.input_shape)
    input_shape[0] = "batch"

    # Detect encoder layer sizes from loaded model
    encoder_layers = []
    for layer in model.layers:
        if hasattr(layer, 'layers'):
            for sub in layer.layers:
                if hasattr(sub, 'units') and 'dense' in sub.name.lower():
                    encoder_layers.append(sub.units)
    if not encoder_layers:
        encoder_layers = [128, 64, 32, 16]

    latent_dim = encoder_layers[-1] if encoder_layers else 8

    metadata = {
        "model_name": "vibration_autoencoder",
        "version": "1.0.0",
        "architecture": "dense_autoencoder",
        "input_shape": input_shape,
        "input_dim": actual_input_dim,
        "latent_dim": latent_dim,
        "encoder_layers": encoder_layers,
        "anomaly_threshold": threshold,
        "anomaly_threshold_percentile": 95.0,
        "anomaly_threshold_note":
            "Reconstruction MSE above this value indicates anomaly. "
            f"Computed on {len(X_cal)} synthetic normal samples at ±0.3σ. "
            "Recalibrate with real normal-condition data for production.",
        "normalization": {
            "mean": [round(float(x), 8) for x in norm_mean[:actual_input_dim]],
            "std":  [round(float(x), 8) for x in norm_std[:actual_input_dim]],
            "feature_names": feature_names,
            "source": "training_data" if Path(__file__).resolve().parents[1] /
                       "models" / "scaler_params.json" else "domain_defaults"
        },
        "training": {
            "source_model": str(H5_PATH),
            "framework": f"TensorFlow {tf.__version__}",
            "loss": "mse",
            "optimizer": "adam"
        }
    }

    with open(METADATA_OUT, "w") as f:
        json.dump(metadata, f, indent=2)
    print(f"Metadata saved: {METADATA_OUT}")

    print("\nDone. To deploy:")
    print(f"  scp {ONNX_OUT} orangepi@192.168.1.1:/opt/edge-gateway/services/inference-engine/src/models/")
    print(f"  scp {METADATA_OUT} orangepi@192.168.1.1:/opt/edge-gateway/services/inference-engine/src/models/")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="EdgeVib Keras -> ONNX Converter")
    parser.add_argument("--h5", type=str, default=None,
                       help="Path to Keras .h5 model (default: autoencoder_24.h5)")
    parser.add_argument("--output", type=str, default=None,
                       help="ONNX output filename (default: autoencoder.onnx)")
    parser.add_argument("--deploy-url", type=str, default=None,
                       help="model-deploy API URL (e.g. http://192.168.1.1:8091)")
    parser.add_argument("--model-name", type=str, default="vibration_autoencoder",
                       help="Model name for deployment (default: vibration_autoencoder)")
    parser.add_argument("--version", type=str, default=None,
                       help="Version tag (default: auto-generated from date)")
    cli_args = parser.parse_args()
    if cli_args.h5:
        H5_PATH = Path(cli_args.h5)
    if cli_args.output:
        ONNX_OUT = MODEL_DIR / cli_args.output
    main()

    # Auto-deploy to model-deploy if requested
    if cli_args.deploy_url and ONNX_OUT.exists():
        import requests as req
        version = cli_args.version or datetime.now().strftime("%Y%m%d.%H%M%S")
        metadata = {}
        metadata_path = METADATA_OUT if METADATA_OUT.exists() else None
        if metadata_path:
            with open(metadata_path) as f:
                metadata = json.load(f)

        with open(ONNX_OUT, "rb") as f:
            files = {"model_file": (ONNX_OUT.name, f, "application/octet-stream")}
            data = {
                "model_name": cli_args.model_name,
                "version": version,
                "platform": "orange-pi",
                "metrics_json": json.dumps({
                    "anomaly_threshold": metadata.get("anomaly_threshold"),
                    "input_dim": metadata.get("input_dim"),
                }),
            }
            try:
                resp = req.post(
                    f"{cli_args.deploy_url.rstrip('/')}/api/v1/models/deploy",
                    files=files, data=data, timeout=60,
                )
                if resp.status_code in (200, 201):
                    print(f"Deployed to model-deploy: {version}")
                else:
                    print(f"Deploy FAILED ({resp.status_code}): {resp.text}")
            except Exception as e:
                print(f"Deploy request failed: {e}")
