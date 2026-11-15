/**
 * Web Worker for client-side photo captioning & OCR via @huggingface/transformers.
 * Uses onnx-community/Florence-2-base-ft cached in the browser's Cache API.
 */
import {
  AutoProcessor,
  AutoTokenizer,
  Florence2ForConditionalGeneration,
  RawImage,
  env,
} from "@huggingface/transformers";
import { configureLocalOrtWasm } from "./ort-wasm-env.js";

configureLocalOrtWasm(env);

const MODEL_ID = "onnx-community/Florence-2-base-ft";
let bundlePromise = null;

async function loadModelWithFallback(progressCallback) {
  const attempts = [
    {
      embed_tokens: "q8",
      vision_encoder: "q8",
      encoder_model: "q4",
      decoder_model_merged: "q4",
    },
    "q4",
    "q8",
    undefined,
  ];
  let lastErr = null;
  for (const dtype of attempts) {
    try {
      const opts = dtype ? { dtype, progress_callback: progressCallback } : { progress_callback: progressCallback };
      return await Florence2ForConditionalGeneration.from_pretrained(MODEL_ID, opts);
    } catch (e) {
      lastErr = e;
    }
  }
  throw lastErr || new Error("Failed to load Florence-2 model");
}

async function getBundle() {
  if (!bundlePromise) {
    const fileProgress = new Map();
    const emitProgress = (info) => {
      if (!info) return;
      if (info.status === "progress" && info.file) {
        const pct = typeof info.progress === "number" ? info.progress : 0;
        fileProgress.set(info.file, pct);
        const vals = Array.from(fileProgress.values());
        const avg = vals.length ? vals.reduce((a, b) => a + b, 0) / vals.length : pct;
        self.postMessage({
          type: "progress",
          file: info.file.split("/").pop() || info.file,
          progress: Math.min(99, Math.round(avg)),
        });
      } else if (info.status === "done" && info.file) {
        fileProgress.set(info.file, 100);
      }
    };

    bundlePromise = Promise.all([
      AutoProcessor.from_pretrained(MODEL_ID, { progress_callback: emitProgress }),
      AutoTokenizer.from_pretrained(MODEL_ID, { progress_callback: emitProgress }),
      loadModelWithFallback(emitProgress),
    ]).then(([processor, tokenizer, model]) => ({ processor, tokenizer, model }));
  }
  return bundlePromise;
}

async function runTask(bundle, image, task, maxNewTokens = 96) {
  const { processor, tokenizer, model } = bundle;
  const prompts = processor.construct_prompts(task);
  const textInputs = tokenizer(prompts);
  const visionInputs = await processor.image_processor(image);
  const generatedIds = await model.generate({
    ...textInputs,
    ...visionInputs,
    max_new_tokens: maxNewTokens,
  });
  const generatedText = tokenizer.batch_decode(generatedIds, {
    skip_special_tokens: false,
  })[0];
  const parsed = processor.post_process_generation(generatedText, task, image.size);
  const val = parsed?.[task];
  return typeof val === "string" ? val.trim() : "";
}

self.onmessage = async (event) => {
  const msg = event.data || {};
  if (msg.type === "init") {
    try {
      await getBundle();
      self.postMessage({ type: "ready" });
    } catch (e) {
      bundlePromise = null;
      self.postMessage({ type: "error", error: e?.message || String(e) });
    }
    return;
  }

  if (msg.type === "recognize") {
    const { id, jpegBytes } = msg;
    try {
      const bundle = await getBundle();
      const blob = new Blob([jpegBytes], { type: "image/jpeg" });
      const image = await RawImage.fromBlob(blob);

      const caption = await runTask(bundle, image, "<DETAILED_CAPTION>", 96);
      const ocr = await runTask(bundle, image, "<OCR>", 96);

      self.postMessage({ type: "recognize-result", id, caption, ocr });
    } catch (e) {
      self.postMessage({ type: "recognize-error", id, error: e?.message || String(e) });
    }
  }
};

