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
import { configureLocalOrtWasm, createModelProgressEmitter } from "./ort-wasm-env.js";

configureLocalOrtWasm(env);

const MODEL_ID = "onnx-community/Florence-2-base-ft";
let bundlePromise = null;

async function loadModelWithFallback(progressCallback) {
  // Always use WASM on mobile/WebView: Adreno Vulkan WebGPU compute shader
  // compiler (libllvm-qgl.so) can SIGSEGV in vkCreateComputePipelines.
  const attempts = [
    {
      dtype: {
        embed_tokens: "q8",
        vision_encoder: "q8",
        encoder_model: "q4",
        decoder_model_merged: "q4",
      },
    },
    { dtype: "q4" },
    { dtype: "q8" },
    {},
  ];

  let lastErr = null;
  for (const cfg of attempts) {
    try {
      return await Florence2ForConditionalGeneration.from_pretrained(MODEL_ID, {
        ...cfg,
        progress_callback: progressCallback,
      });
    } catch (e) {
      lastErr = e;
    }
  }
  throw lastErr || new Error("Failed to load Florence-2 model");
}

async function getBundle() {
  if (!bundlePromise) {
    const emitProgress = createModelProgressEmitter((msg) => self.postMessage(msg));

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

      const t0 = performance.now();
      const caption = await runTask(bundle, image, "<CAPTION>", 36);
      console.log(`[local-vision] captioned in ${Math.round(performance.now() - t0)}ms:`, caption);

      self.postMessage({ type: "recognize-result", id, caption, ocr: "" });
    } catch (e) {
      self.postMessage({ type: "recognize-error", id, error: e?.message || String(e) });
    }
  }
};

