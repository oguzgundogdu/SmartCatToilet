"""Training script for the cat identification model.

Uses MobileNetV2 with transfer learning to classify Maya vs Bella.
Run on a dev machine with TensorFlow installed (not needed on the RPi).

Usage:
    python train.py                          # Train with default settings
    python train.py --data-dir ./data        # Custom data directory
    python train.py --epochs-top 15 --epochs-fine 15  # More epochs

Expected data layout:
    data/
      bella/   (50-100+ JPEG images)
      maya/    (50-100+ JPEG images)

Outputs:
    model/cat_model.tflite   (deploy this to the RPi)
    model/cat_model.keras    (full model for later fine-tuning)
"""

import argparse
from pathlib import Path

import tensorflow as tf
from tensorflow.keras.applications import MobileNetV2
from tensorflow.keras.layers import Dense, Dropout, GlobalAveragePooling2D
from tensorflow.keras.models import Model
from tensorflow.keras.preprocessing.image import ImageDataGenerator

IMG_SIZE = (224, 224)
BATCH_SIZE = 16


def build_model() -> Model:
    base = MobileNetV2(
        weights="imagenet", include_top=False, input_shape=(*IMG_SIZE, 3)
    )
    base.trainable = False

    x = base.output
    x = GlobalAveragePooling2D()(x)
    x = Dropout(0.3)(x)
    output = Dense(2, activation="softmax")(x)

    return Model(inputs=base.input, outputs=output)


def create_generators(data_dir: str):
    datagen = ImageDataGenerator(
        rescale=1.0 / 255,
        rotation_range=20,
        width_shift_range=0.2,
        height_shift_range=0.2,
        horizontal_flip=True,
        brightness_range=[0.7, 1.3],
        zoom_range=0.2,
        validation_split=0.2,
    )

    train_gen = datagen.flow_from_directory(
        data_dir,
        target_size=IMG_SIZE,
        batch_size=BATCH_SIZE,
        class_mode="categorical",
        subset="training",
    )
    val_gen = datagen.flow_from_directory(
        data_dir,
        target_size=IMG_SIZE,
        batch_size=BATCH_SIZE,
        class_mode="categorical",
        subset="validation",
    )

    print(f"Classes: {train_gen.class_indices}")
    print(f"Training samples: {train_gen.samples}")
    print(f"Validation samples: {val_gen.samples}")

    return train_gen, val_gen


def train(data_dir: str, epochs_top: int, epochs_fine: int, output_dir: str):
    train_gen, val_gen = create_generators(data_dir)
    model = build_model()

    # Phase 1: Train top layers only (base frozen)
    print("\n=== Phase 1: Training top layers ===")
    model.compile(
        optimizer="adam", loss="categorical_crossentropy", metrics=["accuracy"]
    )
    model.fit(train_gen, validation_data=val_gen, epochs=epochs_top)

    # Phase 2: Unfreeze last 20 layers and fine-tune with low LR
    print("\n=== Phase 2: Fine-tuning last 20 layers ===")
    base = model.layers[1]  # MobileNetV2 base
    base.trainable = True
    for layer in base.layers[:-20]:
        layer.trainable = False

    model.compile(
        optimizer=tf.keras.optimizers.Adam(1e-5),
        loss="categorical_crossentropy",
        metrics=["accuracy"],
    )
    model.fit(train_gen, validation_data=val_gen, epochs=epochs_fine)

    # Save full Keras model (for future fine-tuning)
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)

    keras_path = out / "cat_model.keras"
    model.save(keras_path)
    print(f"\nKeras model saved: {keras_path}")

    # Convert to TFLite with float16 quantization
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.target_spec.supported_types = [tf.float16]
    tflite_model = converter.convert()

    tflite_path = out / "cat_model.tflite"
    tflite_path.write_bytes(tflite_model)
    print(f"TFLite model saved: {tflite_path} ({len(tflite_model) / 1024:.0f} KB)")
    print(f"\nDeploy {tflite_path} to the RPi classifier service.")


def main():
    parser = argparse.ArgumentParser(description="Train cat identification model")
    parser.add_argument(
        "--data-dir", default="data", help="Training data directory (default: data)"
    )
    parser.add_argument(
        "--epochs-top",
        type=int,
        default=10,
        help="Epochs for top-layer training (default: 10)",
    )
    parser.add_argument(
        "--epochs-fine",
        type=int,
        default=10,
        help="Epochs for fine-tuning (default: 10)",
    )
    parser.add_argument(
        "--output-dir",
        default="model",
        help="Output directory for models (default: model)",
    )
    args = parser.parse_args()

    data_path = Path(args.data_dir)
    if not (data_path / "maya").exists() or not (data_path / "bella").exists():
        print(
            f"Error: Expected {data_path}/maya/ and {data_path}/bella/ directories "
            f"with training images.\n"
            f"Collect images using the /upload-training endpoint or manually."
        )
        raise SystemExit(1)

    train(args.data_dir, args.epochs_top, args.epochs_fine, args.output_dir)


if __name__ == "__main__":
    main()
