"""TFLite MobileNetV2 inference wrapper for cat identification."""

import io
import logging

import numpy as np
from PIL import Image

logger = logging.getLogger(__name__)

LABELS = ["bella", "maya"]  # Alphabetical — must match training class order
INPUT_SIZE = (224, 224)
CONFIDENCE_THRESHOLD = 0.65


class CatClassifier:
    def __init__(self, model_path: str):
        # Import here so the module can be imported even without the runtime
        # (useful for training-only environments).
        try:
            from ai_edge_litert.interpreter import Interpreter
        except ImportError:
            from tflite_runtime.interpreter import Interpreter

        self.interpreter = Interpreter(model_path=model_path)
        self.interpreter.allocate_tensors()
        self.input_details = self.interpreter.get_input_details()
        self.output_details = self.interpreter.get_output_details()
        logger.info(
            "Model loaded: %s, input shape: %s",
            model_path,
            self.input_details[0]["shape"],
        )

    def predict(self, image_bytes: bytes) -> tuple[str, float]:
        """Classify a JPEG image as bella or maya.

        Returns (label, confidence). If confidence is below threshold,
        returns ("unknown", confidence_of_best_class).
        """
        img = Image.open(io.BytesIO(image_bytes)).convert("RGB")
        img = img.resize(INPUT_SIZE)
        input_data = np.expand_dims(
            np.array(img, dtype=np.float32) / 255.0, axis=0
        )

        self.interpreter.set_tensor(self.input_details[0]["index"], input_data)
        self.interpreter.invoke()
        output = self.interpreter.get_tensor(self.output_details[0]["index"])[0]

        idx = int(np.argmax(output))
        confidence = float(output[idx])

        if confidence < CONFIDENCE_THRESHOLD:
            logger.info(
                "Low confidence: best=%s (%.3f), returning unknown",
                LABELS[idx],
                confidence,
            )
            return "unknown", confidence

        return LABELS[idx], confidence
