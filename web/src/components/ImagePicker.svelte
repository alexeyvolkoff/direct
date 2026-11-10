<script>
  import { createEventDispatcher } from "svelte";
  import FolderPicker from "./FolderPicker.svelte";
  import { IMAGE_ACCEPT } from "../lib/file-accept.js";

  let { device = null, title = "Select Image" } = $props();

  const dispatch = createEventDispatcher();
</script>

<FolderPicker
  {device}
  mode="file"
  accept={IMAGE_ACCEPT}
  allowUpload={true}
  uploadAccept={IMAGE_ACCEPT.join(",")}
  {title}
  on:select={(e) => dispatch("select", e.detail)}
  on:cancel={() => dispatch("cancel")}
/>