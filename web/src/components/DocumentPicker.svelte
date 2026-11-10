<script>
  import { createEventDispatcher } from "svelte";
  import FolderPicker from "./FolderPicker.svelte";
  import { DOCUMENT_ACCEPT } from "../lib/file-accept.js";

  let { device = null, title = "Select Document" } = $props();

  const dispatch = createEventDispatcher();
</script>

<FolderPicker
  {device}
  mode="file"
  accept={DOCUMENT_ACCEPT}
  allowUpload={true}
  uploadAccept={DOCUMENT_ACCEPT.join(",")}
  {title}
  on:select={(e) => dispatch("select", e.detail)}
  on:cancel={() => dispatch("cancel")}
/>
