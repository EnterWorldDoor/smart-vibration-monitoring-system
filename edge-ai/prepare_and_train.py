"""
======================================================================
 EdgeVib PC Training Pipeline — ESP32-aligned [time_steps, 24]
 Usage: conda activate edgevib-tf && python prepare_and_train.py

 Produces model with input shape matching ESP32 ai_service.h:
   [AI_FEATURE_WINDOWS, AI_NUM_FEATURES] = [time_steps, 24]
======================================================================
"""

import csv, math, os, sys, json, warnings, shutil
import numpy as np
import pandas as pd
from datetime import datetime
from collections import Counter

os.environ['TF_CPP_MIN_LOG_LEVEL'] = '2'
warnings.filterwarnings('ignore')

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE_DIR)

CSV_PATH   = os.path.join(BASE_DIR, 'data_collection', 'training_data.csv')
BAK_PATH   = os.path.join(BASE_DIR, 'data_collection', 'training_data.csv.bak')
SAVED_DIR  = os.path.join(BASE_DIR, 'models', 'saved_models')
DEPLOY_DIR = os.path.join(BASE_DIR, 'deployment', 'models')
LOG_DIR    = os.path.join(BASE_DIR, 'logs')

for d in [SAVED_DIR, DEPLOY_DIR, LOG_DIR]:
    os.makedirs(d, exist_ok=True)

# ================================================================
# ESP32-aligned constants (must match ai_service.h after update)
# ================================================================
TIME_STEPS       = 32    # AI_FEATURE_WINDOWS — sequence length (ESP32: 32×160ms≈5s)
NUM_FEATURES     = 24    # AI_NUM_FEATURES — feature vector dim
CONFIDENCE_THRESHOLD = 0.85
OUTPUT_CLASSES   = ['normal', 'imbalance', 'misalignment', 'bearing_fault']

# 8 frequency bands for band_energy (0-200Hz Nyquist @ 400Hz sampling)
FREQ_BANDS = [(0,25),(25,50),(50,75),(75,100),(100,125),(125,150),(150,175),(175,200)]

print("=" * 60)
print(" EdgeVib Training Pipeline — ESP32-aligned [%d, %d]" % (TIME_STEPS, NUM_FEATURES))
print("=" * 60)

# ================================================================
# Step 0: CSV preparation + rule-based pseudo-labeling
# ================================================================
print("\n" + "=" * 60)
print(" Step 0: CSV Preparation + Pseudo-Labeling")
print("=" * 60)

if not os.path.exists(BAK_PATH):
    print("ERROR: backup not found:", BAK_PATH); sys.exit(1)

shutil.copy(BAK_PATH, CSV_PATH)

rows = []
with open(CSV_PATH, 'r') as f:
    for r in csv.DictReader(f):
        r['rms_x'] = float(r['rms_x']); r['rms_y'] = float(r['rms_y'])
        r['rms_z'] = float(r['rms_z']); r['overall_rms'] = float(r['overall_rms'])
        r['peak_freq'] = float(r['peak_freq']); r['peak_amp'] = float(r['peak_amp'])
        r['temperature_c'] = float(r['temperature_c']); r['humidity_rh'] = float(r['humidity_rh'])
        r['total_rms'] = math.sqrt(r['rms_x']**2 + r['rms_y']**2 + r['rms_z']**2)
        rows.append(r)

rows = [r for r in rows if r['temperature_c'] > 0]
for r in rows:
    r['label'] = 'unknown'

high_vib = [r for r in rows if r['total_rms'] > 5]
idle     = [r for r in rows if r['total_rms'] <= 5]

high_vib.sort(key=lambda r: r['peak_freq'])
n = len(high_vib); p25=n//4; p50=n//2; p75=3*n//4
for i, r in enumerate(high_vib):
    if   i < p25:  r['label'] = 'normal'
    elif i < p50:  r['label'] = 'imbalance'
    elif i < p75:  r['label'] = 'misalignment'
    else:          r['label'] = 'bearing_fault'

import random; random.seed(42)
for r in random.sample([x for x in idle if x['peak_freq'] <= 1.0], 8):
    r['label'] = 'normal'

cts = Counter(r['label'] for r in rows)
print("  normal=%d imbalance=%d misalignment=%d bearing_fault=%d unknown=%d" %
      (cts['normal'], cts['imbalance'], cts['misalignment'],
       cts['bearing_fault'], cts['unknown']))

# ================================================================
# Step 1: ESP32-aligned 24-dim feature extraction from CSV rows
# ================================================================
print("\n" + "=" * 60)
print(" Step 1: 24-dim Feature Extraction (ESP32 layout)")
print("=" * 60)

def extract_features_from_rows(row_slice):
    """
    Extract exactly 24 features matching ESP32 ai_service.c push_feature_vector().
    Layout: [0]rms_x [1]rms_y [2]rms_z [3]overall_rms
            [4]peak_freq_x [5]peak_amp_x [6]skewness_x [7]kurtosis_x [8]crest_factor_x
            [9-16]band_energy_x[0..7]
            [17]peak_freq_y [18]peak_amp_y [19]crest_factor_y
            [20]peak_freq_z [21]peak_amp_z [22]crest_factor_z
            [23]temperature_c
    """
    n = len(row_slice)
    rms_x = np.array([r['rms_x'] for r in row_slice], dtype=np.float64)
    rms_y = np.array([r['rms_y'] for r in row_slice], dtype=np.float64)
    rms_z = np.array([r['rms_z'] for r in row_slice], dtype=np.float64)
    temp  = np.array([r['temperature_c'] for r in row_slice], dtype=np.float64)

    feats = np.zeros(NUM_FEATURES, dtype=np.float32)

    # [0-2] RMS per axis
    feats[0] = np.sqrt(np.mean(np.square(rms_x)))
    feats[1] = np.sqrt(np.mean(np.square(rms_y)))
    feats[2] = np.sqrt(np.mean(np.square(rms_z)))

    # [3] overall_rms
    feats[3] = np.sqrt(feats[0]**2 + feats[1]**2 + feats[2]**2)

    def fft_peak(signal, fs=1.0):
        """Return (peak_freq, peak_amp) from FFT of a 1D signal."""
        if len(signal) < 4:
            return 0.0, 0.0
        fft_vals = np.abs(np.fft.rfft(signal))
        freqs = np.fft.rfftfreq(len(signal), d=1.0/fs)
        if len(fft_vals) <= 1:
            return 0.0, 0.0
        peak_idx = np.argmax(fft_vals[1:]) + 1
        return float(freqs[peak_idx]), float(fft_vals[peak_idx])

    def band_energies(signal, fs=1.0):
        """Compute 8 band energy ratios from FFT of a 1D signal."""
        if len(signal) < 4:
            return np.zeros(8, dtype=np.float32)
        fft_vals = np.abs(np.fft.rfft(signal))
        freqs = np.fft.rfftfreq(len(signal), d=1.0/fs)
        total_power = float(np.sum(fft_vals ** 2))
        bands = np.zeros(8, dtype=np.float32)
        if total_power > 1e-10:
            for i, (lo, hi) in enumerate(FREQ_BANDS):
                mask = (freqs >= lo) & (freqs < hi)
                bands[i] = float(np.sum(fft_vals[mask] ** 2)) / total_power
        return bands

    def stats(signal):
        """Return (skewness, kurtosis, crest_factor) for a signal."""
        if len(signal) < 4:
            return 0.0, 0.0, 0.0
        std = np.std(signal)
        if std < 1e-10:
            return 0.0, 0.0, 0.0
        centered = signal - np.mean(signal)
        sk = float(np.mean(centered ** 3) / (std ** 3))
        ku = float(np.mean(centered ** 4) / (std ** 4))
        cf = float(np.max(np.abs(signal)) / (np.sqrt(np.mean(np.square(signal))) + 1e-10))
        return sk, ku, cf

    # X-axis features [4-16]
    pf_x, pa_x = fft_peak(rms_x)
    feats[4] = pf_x
    feats[5] = pa_x
    sk_x, ku_x, cf_x = stats(rms_x)
    feats[6] = sk_x
    feats[7] = ku_x
    feats[8] = cf_x
    feats[9:17] = band_energies(rms_x)

    # Y-axis features [17-19]
    pf_y, pa_y = fft_peak(rms_y)
    feats[17] = pf_y
    feats[18] = pa_y
    _, _, cf_y = stats(rms_y)
    feats[19] = cf_y

    # Z-axis features [20-22]
    pf_z, pa_z = fft_peak(rms_z)
    feats[20] = pf_z
    feats[21] = pa_z
    _, _, cf_z = stats(rms_z)
    feats[22] = cf_z

    # [23] temperature
    feats[23] = float(np.mean(temp)) if len(temp) > 0 else 25.0

    return feats


# ================================================================
# Step 1-2: Feature extraction centered on labeled rows + augmentation
# ================================================================
print("\n" + "=" * 60)
print(" Step 1-2: Feature Vectors Centered on Labeled Rows")
print("=" * 60)

# Strategy: extract feature vectors from windows centered on labeled rows,
# then augment with noise to create enough data for time-series windows.

FEAT_WINDOW = 8        # CSV rows per feature vector
FEAT_STRIDE = 1        # dense extraction around labeled rows

feature_vectors = []
feature_labels  = []

# Extract feature vectors from ALL windows, then filter for known labels
for start in range(0, len(rows) - FEAT_WINDOW + 1, FEAT_STRIDE):
    end = start + FEAT_WINDOW
    window_rows = rows[start:end]
    feat = extract_features_from_rows(window_rows)
    feature_vectors.append(feat)
    known_labels = [r['label'] for r in window_rows if r['label'] != 'unknown']
    if known_labels:
        feature_labels.append(Counter(known_labels).most_common(1)[0][0])
    else:
        feature_labels.append('unknown')

feature_vectors = np.array(feature_vectors, dtype=np.float32)
feature_labels  = np.array(feature_labels)
print("  All feature windows: %d x %d" % (len(feature_vectors), NUM_FEATURES))
print("  Label distribution: %s" % dict(Counter(feature_labels)))

# Separate labeled and unlabeled
labeled_mask = np.array([l != 'unknown' for l in feature_labels])
X_labeled = feature_vectors[labeled_mask]
y_labeled = feature_labels[labeled_mask]

print("  Labeled feature vectors: %d" % len(X_labeled))
print("  Labeled class dist: %s" % dict(Counter(y_labeled)))

# Augment labeled feature vectors with noise to reach target per class
n_target_per_class = 200  # target feature vectors per class

X_aug_feats = []
y_aug_feats = []
rng = np.random.RandomState(42)

for cls in OUTPUT_CLASSES:
    cls_mask = y_labeled == cls
    X_cls = X_labeled[cls_mask]
    n_cls = len(X_cls)
    if n_cls == 0:
        continue
    n_needed = n_target_per_class - n_cls
    n_copies = max(1, n_needed // max(n_cls, 1))

    for idx in range(n_cls):
        # Original
        X_aug_feats.append(X_cls[idx])
        y_aug_feats.append(cls)
        # Noisy copies
        for c in range(n_copies):
            noise = rng.normal(0, 0.03 * (np.abs(X_cls[idx]) + 0.05), X_cls[idx].shape)
            X_aug_feats.append(np.clip(X_cls[idx] + noise, -5.0, 5.0))
            y_aug_feats.append(cls)

X_aug_feats = np.array(X_aug_feats, dtype=np.float32)
y_aug_feats = np.array(y_aug_feats)

print("  After augmentation: %d feature vectors" % len(X_aug_feats))
print("  Augmented class dist: %s" % dict(Counter(y_aug_feats)))

# ================================================================
# Step 3: Z-score normalization + time-series window construction
# ================================================================
print("\n" + "=" * 60)
print(" Step 3: Normalization + Time-Series Windows [%d, %d]" % (TIME_STEPS, NUM_FEATURES))
print("=" * 60)

# Fit normalizer on ALL feature vectors (not just labeled)
f_mean = feature_vectors.mean(axis=0)
f_std  = feature_vectors.std(axis=0)
f_std[f_std < 1e-8] = 1.0

# Normalize augmented labeled features
X_aug_norm = (X_aug_feats - f_mean) / f_std

# Build time-series windows: slide over augmented feature vectors
# Use class-aware sampling: ensure each window's center feature belongs to a target class
from sklearn.preprocessing import LabelEncoder
le = LabelEncoder()
y_enc = le.fit_transform([l if l in OUTPUT_CLASSES else 'unknown' for l in y_aug_feats])

X_wins = []
y_wins = []

half_win = TIME_STEPS // 2
win_stride = max(TIME_STEPS // 8, 1)

# Build windows centered on each augmented feature vector
for center in range(half_win, len(X_aug_norm) - half_win, win_stride):
    start = center - half_win
    end = start + TIME_STEPS
    if end > len(X_aug_norm):
        continue
    X_wins.append(X_aug_norm[start:end])
    # Label from center feature vector
    y_wins.append(y_enc[center])

X_wins = np.array(X_wins, dtype=np.float32)
y_wins = np.array(y_wins, dtype=np.int32)
class_names = list(le.classes_)

print("  Time-series windows: %d  shape=%s" % (len(X_wins), X_wins.shape))
print("  Classes: %s -> %s" % (class_names, list(range(len(class_names)))))
print("  Class distribution: %s" % dict(Counter(y_wins)))

# ================================================================
# Step 4: Train/val/test split + 1D-CNN model
# ================================================================
print("\n" + "=" * 60)
print(" Step 4: 1D-CNN Model Training (no LSTM — TFLite Micro safe)")
print("=" * 60)

n_w = len(X_wins)
perm = np.random.RandomState(42).permutation(n_w)
n_ts = max(n_w // 6, 1)
n_vl = max(n_w // 6, 1)
n_tr = n_w - n_ts - n_vl

X_tr = X_wins[perm[:n_tr]]
y_tr = y_wins[perm[:n_tr]]
X_vl = X_wins[perm[n_tr:n_tr+n_vl]]
y_vl = y_wins[perm[n_tr:n_tr+n_vl]]
X_ts = X_wins[perm[n_tr+n_vl:]]
y_ts = y_wins[perm[n_tr+n_vl:]]
print("  Split: train=%d val=%d test=%d" % (len(X_tr), len(X_vl), len(X_ts)))

import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

num_classes = len(class_names)

# 1D-CNN model — no LSTM, safe for TFLite Micro
input_layer = layers.Input(shape=(TIME_STEPS, NUM_FEATURES), name='input_features')

x = layers.Conv1D(32, 5, padding='same', activation='relu', name='conv1')(input_layer)
x = layers.MaxPooling1D(2, name='pool1')(x)
x = layers.Conv1D(64, 5, padding='same', activation='relu', name='conv2')(x)
x = layers.MaxPooling1D(2, name='pool2')(x)
x = layers.Conv1D(128, 3, padding='same', activation='relu', name='conv3')(x)
x = layers.GlobalAveragePooling1D(name='gap')(x)
x = layers.Dropout(0.4, name='dropout')(x)
x = layers.Dense(64, activation='relu', name='dense1')(x)
output_layer = layers.Dense(num_classes, activation='softmax', name='output')(x)

model = keras.Model(inputs=input_layer, outputs=output_layer, name='edgevib_cnn')
model.compile(
    optimizer=keras.optimizers.Adam(learning_rate=0.001),
    loss='sparse_categorical_crossentropy',
    metrics=['accuracy']
)
model.summary()

# Train
early_stop = keras.callbacks.EarlyStopping(
    monitor='val_loss', patience=20, restore_best_weights=True)
reduce_lr = keras.callbacks.ReduceLROnPlateau(
    monitor='val_loss', factor=0.5, patience=8, min_lr=1e-5)

history = model.fit(
    X_tr, y_tr,
    validation_data=(X_vl, y_vl),
    epochs=150,
    batch_size=16,
    callbacks=[early_stop, reduce_lr],
    verbose=1
)

# Evaluate
test_loss, test_acc = model.evaluate(X_ts, y_ts, verbose=0)
print("\n  Test accuracy (heuristic-label): %.4f" % test_acc)

# Per-class metrics
y_pred_probs = model.predict(X_ts, verbose=0)
y_pred = np.argmax(y_pred_probs, axis=1)
from sklearn.metrics import classification_report
print("\n" + classification_report(
    y_ts, y_pred, target_names=class_names, zero_division=0))

# Save Keras model
h5_path = os.path.join(SAVED_DIR, 'edgevib_cnn.h5')
model.save(h5_path)
print("  Saved: %s" % h5_path)

# ================================================================
# Step 5: TFLite Conversion — pure TFLITE_BUILTINS, INT8-ready
# ================================================================
print("\n" + "=" * 60)
print(" Step 5: TFLite Conversion (TFLITE_BUILTINS only)")
print("=" * 60)

try:
    converter = tf.lite.TFLiteConverter.from_keras_model(model)

    # Pure FP32 conversion — no optimizations (ESP-NN doesn't support hybrid models)
    # converter.optimizations = []  # deliberately empty: avoid hybrid model error on TFLite Micro
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS]

    tflite_bytes = converter.convert()
    tflite_path = os.path.join(DEPLOY_DIR, 'edgevib_classifier.tflite')

    with open(tflite_path, 'wb') as f:
        f.write(tflite_bytes)

    print("  TFLite size: %d bytes (%.1f KB)" % (len(tflite_bytes), len(tflite_bytes)/1024.0))

    # Verify on PC
    interpreter = tf.lite.Interpreter(model_content=tflite_bytes)
    interpreter.allocate_tensors()
    inp_det = interpreter.get_input_details()[0]
    out_det = interpreter.get_output_details()[0]
    print("  TFLite input:  %s" % inp_det['shape'])
    print("  TFLite output: %s" % out_det['shape'])

    # Accuracy check
    tflite_ok = 0
    n_check = min(50, len(X_ts))
    for i in range(n_check):
        sample = X_ts[i:i+1].astype(np.float32)
        interpreter.set_tensor(inp_det['index'], sample)
        interpreter.invoke()
        tflite_out = interpreter.get_tensor(out_det['index'])[0]
        if np.argmax(tflite_out) == y_ts[i]:
            tflite_ok += 1
    print("  TFLite accuracy: %d/%d = %.4f (vs Keras: %.4f)" %
          (tflite_ok, n_check, tflite_ok/n_check, test_acc))

    # Benchmark
    import time
    times_ms = []
    warmup = X_tr[0:1].astype(np.float32)
    for _ in range(10):
        interpreter.set_tensor(inp_det['index'], warmup)
        interpreter.invoke()
    for _ in range(30):
        t0 = time.perf_counter()
        interpreter.set_tensor(inp_det['index'], warmup)
        interpreter.invoke()
        times_ms.append((time.perf_counter() - t0) * 1000)
    print("  Avg inference: %.1f ms (PC, FP32 TFLite)" % np.mean(times_ms))

except Exception as e:
    print("  TFLite FAILED: %s" % e)
    import traceback; traceback.print_exc()
    tflite_path = None

# ================================================================
# Step 6: C Header Generation for ESP32
# ================================================================
print("\n" + "=" * 60)
print(" Step 6: C Header Generation")
print("=" * 60)

if tflite_path and os.path.exists(tflite_path):
    hdr_path = os.path.join(DEPLOY_DIR, 'model_data.h')
    with open(tflite_path, 'rb') as f:
        model_bytes = f.read()

    with open(hdr_path, 'w') as f:
        f.write("// EdgeVib Auto-generated Model — %s\n" % datetime.now().isoformat())
        f.write("// Input: [%d, %d]  Classes: %s\n" % (TIME_STEPS, NUM_FEATURES, class_names))
        f.write("// Keras accuracy (heuristic-label): %.4f\n\n" % test_acc)
        f.write("#ifndef EDGEVIB_MODEL_DATA_H\n#define EDGEVIB_MODEL_DATA_H\n\n")
        f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        f.write("alignas(16) static const unsigned char model_data[] = {\n  ")
        for i, b in enumerate(model_bytes):
            f.write("0x%02x, " % b)
            if (i + 1) % 12 == 0:
                f.write("\n  ")
        f.write("\n};\n\n")
        f.write("static const unsigned int model_data_len = %d;\n\n" % len(model_bytes))
        f.write("#ifdef __cplusplus\n}\n#endif\n")
        f.write("#endif /* EDGEVIB_MODEL_DATA_H */\n")

    print("  Generated: %s" % hdr_path)
    print("  Size: %d bytes (%.1f KB)" % (len(model_bytes), len(model_bytes)/1024.0))

    # Also save feature normalization params for ESP32
    norm_path = os.path.join(DEPLOY_DIR, 'feature_norm.json')
    with open(norm_path, 'w') as f:
        json.dump({
            'input_shape': [TIME_STEPS, NUM_FEATURES],
            'classes': class_names,
            'feature_mean': f_mean.tolist(),
            'feature_std': f_std.tolist(),
            'keras_accuracy': float(test_acc),
            'timestamp': datetime.now().isoformat()
        }, f, indent=2)
    print("  Normalization params: %s" % norm_path)

else:
    print("  SKIPPED (no TFLite model)")

# ================================================================
# Done
# ================================================================
print("\n" + "=" * 60)
print(" PIPELINE COMPLETE")
print("=" * 60)
print("  Model input:  [%d, %d]" % (TIME_STEPS, NUM_FEATURES))
print("  Keras model:  %s" % h5_path)
if tflite_path:
    print("  TFLite model: %s" % tflite_path)
    print("  C Header:     %s" % hdr_path)
    print("  Norm params:  %s" % norm_path)
print("  Next: update ai_service.h AI_FEATURE_WINDOWS=%d, AI_NUM_FEATURES=%d" %
      (TIME_STEPS, NUM_FEATURES))
print("        copy model_data.h to firmware/esp32-gateway/components/ai_service/")

# ================================================================
# Step 7: 24-Dim Autoencoder Training (for Orange Pi inference-engine)
# ================================================================
print("\n" + "=" * 60)
print(" Step 7: 24-Dim Autoencoder Training (ESP32-Aligned)")
print("=" * 60)

# Use only NORMAL-class feature vectors for unsupervised autoencoder training
normal_mask = np.array([l == 'normal' for l in y_aug_feats])
X_normal = X_aug_norm[normal_mask]
print("  Normal-class feature vectors: %d" % len(X_normal))

if len(X_normal) > 100:
    # Train/val split
    n_ae = len(X_normal)
    n_ae_val = max(n_ae // 5, 1)
    perm_ae = np.random.RandomState(42).permutation(n_ae)
    X_ae_tr = X_normal[perm_ae[:-n_ae_val]]
    X_ae_vl = X_normal[perm_ae[-n_ae_val:]]

    from tensorflow import keras
    from tensorflow.keras import layers

    # Build autoencoder matching VibrationAutoencoder architecture
    ae_input = layers.Input(shape=(NUM_FEATURES,), name='autoencoder_input')
    x = ae_input
    encoder_layers = [128, 64, 32, 16]
    for units in encoder_layers:
        x = layers.Dense(units, activation='relu', name=f'enc_dense_{units}')(x)
        x = layers.BatchNormalization(name=f'enc_bn_{units}')(x)
    latent = layers.Dense(8, activation='relu', name='latent')(x)

    # Decoder (mirror)
    dec_input = layers.Input(shape=(8,), name='decoder_input')
    y = dec_input
    for units in reversed(encoder_layers):
        y = layers.Dense(units, activation='relu', name=f'dec_dense_{units}')(y)
        y = layers.BatchNormalization(name=f'dec_bn_{units}')(y)
    dec_output = layers.Dense(NUM_FEATURES, activation='linear', name='decoder_output')(y)

    decoder = keras.Model(dec_input, dec_output, name='decoder')
    reconstructed = decoder(latent)
    autoencoder = keras.Model(ae_input, reconstructed, name='vibration_autoencoder_24')

    autoencoder.compile(
        optimizer=keras.optimizers.Adam(learning_rate=0.001),
        loss='mse',
        metrics=['mae']
    )
    autoencoder.summary()

    # Train
    ae_callbacks = [
        keras.callbacks.EarlyStopping(
            monitor='val_loss', patience=30, restore_best_weights=True, verbose=1),
        keras.callbacks.ReduceLROnPlateau(
            monitor='val_loss', factor=0.5, patience=10, min_lr=1e-6, verbose=1),
    ]

    ae_history = autoencoder.fit(
        X_ae_tr, X_ae_tr,
        validation_data=(X_ae_vl, X_ae_vl),
        epochs=200,
        batch_size=16,
        callbacks=ae_callbacks,
        verbose=1
    )

    # Compute best val loss
    best_val_loss = float(np.min(ae_history.history['val_loss']))
    print("  Autoencoder best val_loss: %.6f" % best_val_loss)

    # Save
    ae_path = os.path.join(SAVED_DIR, 'autoencoder_24.h5')
    autoencoder.save(ae_path)
    print("  Saved: %s" % ae_path)

    # Save normalization params alongside model
    ae_norm_path = os.path.join(SAVED_DIR, 'autoencoder_24_norm.json')
    with open(ae_norm_path, 'w') as f:
        json.dump({
            'input_dim': NUM_FEATURES,
            'feature_mean': f_mean.tolist(),
            'feature_std': f_std.tolist(),
            'val_loss': round(best_val_loss, 6),
            'feature_names': [
                "rms_x", "rms_y", "rms_z", "overall_rms",
                "peak_freq_x", "peak_amp_x", "skewness_x", "kurtosis_x", "crest_factor_x",
                "band_energy_x_0", "band_energy_x_1", "band_energy_x_2", "band_energy_x_3",
                "band_energy_x_4", "band_energy_x_5", "band_energy_x_6", "band_energy_x_7",
                "peak_freq_y", "peak_amp_y", "crest_factor_y",
                "peak_freq_z", "peak_amp_z", "crest_factor_z",
                "temperature_c"
            ],
            'timestamp': datetime.now().isoformat()
        }, f, indent=2)
    print("  Normalization params: %s" % ae_norm_path)

else:
    print("  WARNING: Not enough normal samples for autoencoder training (%d)" % len(X_normal))

# ================================================================
# Done
# ================================================================
print("\n" + "=" * 60)
print(" PIPELINE COMPLETE")
print("=" * 60)
print("  Model input:  [%d, %d]" % (TIME_STEPS, NUM_FEATURES))
print("  Keras model:  %s" % h5_path)
if tflite_path:
    print("  TFLite model: %s" % tflite_path)
    print("  C Header:     %s" % hdr_path)
    print("  Norm params:  %s" % norm_path)
print("  Autoencoder:  models/saved_models/autoencoder_24.h5" if len(X_normal) > 100 else "  Autoencoder:  SKIPPED (insufficient data)")
print("  Next: update ai_service.h AI_FEATURE_WINDOWS=%d, AI_NUM_FEATURES=%d" %
      (TIME_STEPS, NUM_FEATURES))
print("        copy model_data.h to firmware/esp32-gateway/components/ai_service/")
