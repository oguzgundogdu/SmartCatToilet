"""Cat identification classifier service for SmartCatToilet V2.

Receives JPEG images from the ESP32-CAM, runs TFLite inference to identify
which cat (Maya or Bella), and forwards the result to Home Assistant.

Run with: uvicorn app:app --host 0.0.0.0 --port 5000
"""

import logging
import os
import time
from pathlib import Path

import httpx
from fastapi import BackgroundTasks, FastAPI, File, Form, UploadFile
from fastapi.responses import JSONResponse

from model import CatClassifier

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)

MODEL_PATH = os.environ.get("MODEL_PATH", "model/cat_model.tflite")
HA_WEBHOOK_URL = os.environ.get(
    "HA_WEBHOOK_URL", "http://192.168.1.110:8123/api/webhook/cat_toilet"
)
TRAINING_DATA_DIR = Path(os.environ.get("TRAINING_DATA_DIR", "data"))
UNCERTAIN_DIR = TRAINING_DATA_DIR / "uncertain"

app = FastAPI(title="Cat Toilet Classifier")

# Lazy-loaded model (allows startup without a trained model for data collection)
classifier: CatClassifier | None = None

# In-memory store for latest classification result
latest_result: dict = {
    "cat_id": "unknown",
    "confidence": 0.0,
    "timestamp": 0,
}


@app.on_event("startup")
def load_model():
    global classifier
    if Path(MODEL_PATH).exists():
        classifier = CatClassifier(MODEL_PATH)
        logger.info("Classifier ready with model: %s", MODEL_PATH)
    else:
        logger.warning(
            "No model found at %s — /classify will return errors. "
            "Use /upload-training to collect data and train first.",
            MODEL_PATH,
        )


async def notify_ha(cat_id: str, confidence: float):
    """POST classification result to HA webhook (fire-and-forget)."""
    payload = {
        "event": "CAT_IMAGE_RESULT",
        "cat_id": cat_id,
        "confidence": round(confidence, 3),
        "ts": int(time.time()),
    }
    try:
        async with httpx.AsyncClient() as client:
            resp = await client.post(HA_WEBHOOK_URL, json=payload, timeout=5.0)
            logger.info("HA notified: %s (status %d)", cat_id, resp.status_code)
    except Exception:
        logger.exception("Failed to notify HA")


@app.post("/classify")
async def classify(background_tasks: BackgroundTasks, image: UploadFile = File(...)):
    """Receive a JPEG from ESP32-CAM, classify, store result, notify HA."""
    global latest_result

    if classifier is None:
        return JSONResponse(
            status_code=503,
            content={"error": "No model loaded. Train and deploy a model first."},
        )

    image_bytes = await image.read()
    cat_id, confidence = classifier.predict(image_bytes)

    latest_result = {
        "cat_id": cat_id,
        "confidence": round(confidence, 3),
        "timestamp": int(time.time()),
    }
    logger.info("Classified: %s (%.3f)", cat_id, confidence)

    # Save uncertain images for later labeling
    if cat_id == "unknown":
        _save_image(image_bytes, UNCERTAIN_DIR)

    background_tasks.add_task(notify_ha, cat_id, confidence)

    return {"cat_id": cat_id, "confidence": round(confidence, 3)}


@app.get("/latest")
async def latest():
    """Return the most recent classification result."""
    age = int(time.time()) - latest_result["timestamp"]
    if latest_result["timestamp"] == 0 or age > 300:
        return {"cat_id": "unknown", "confidence": 0.0, "age_seconds": age}
    return {**latest_result, "age_seconds": age}


@app.post("/upload-training")
async def upload_training(
    image: UploadFile = File(...),
    label: str = Form(...),
):
    """Save a labeled image for training data collection.

    label must be 'maya' or 'bella'.
    """
    if label not in ("maya", "bella"):
        return JSONResponse(
            status_code=400,
            content={"error": "label must be 'maya' or 'bella'"},
        )

    image_bytes = await image.read()
    label_dir = TRAINING_DATA_DIR / label
    path = _save_image(image_bytes, label_dir)

    return {"saved": str(path), "label": label}


@app.get("/health")
async def health():
    """Service health check."""
    return {
        "status": "ok",
        "model_loaded": classifier is not None,
        "model_path": MODEL_PATH,
        "latest_classification_age": int(time.time()) - latest_result["timestamp"]
        if latest_result["timestamp"] > 0
        else None,
    }


def _save_image(image_bytes: bytes, directory: Path) -> Path:
    """Save image bytes to a directory with a timestamped filename."""
    directory.mkdir(parents=True, exist_ok=True)
    filename = f"{int(time.time() * 1000)}.jpg"
    path = directory / filename
    path.write_bytes(image_bytes)
    logger.info("Saved image: %s (%d bytes)", path, len(image_bytes))
    return path
